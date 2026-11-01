#include "engine/strata_proto.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace judas {

namespace {

std::string fmt_g(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.9g", v);
    return b;
}

std::string home_expand(const std::string & p) {
    if (!p.empty() && p[0] == '~' && (p.size() == 1 || p[1] == '/')) {
        const char * h = std::getenv("HOME");
        return std::string(h ? h : "") + p.substr(1);
    }
    return p;
}

std::vector<std::string> split_ws(const std::string & s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ') j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool is_int(const std::string & s) {
    if (s.empty()) return false;
    size_t i = s[0] == '-' ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); i++)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

}  // namespace

StrataSampling strata_sampling(float req_temp, float req_top_p, float req_min_p, int req_top_k, unsigned req_seed,
                               const ModelConfig & m) {
    StrataSampling s;
    s.temp = req_temp >= 0 ? req_temp : m.temp;
    s.top_p = req_top_p >= 0 ? req_top_p : m.top_p;
    s.min_p = req_min_p >= 0 ? req_min_p : m.min_p;
    s.top_k = req_top_k >= 0 ? req_top_k : m.top_k;
    s.seed = req_seed == 0xFFFFFFFFu ? 0 : req_seed;
    return s;
}

std::string strata_gen_line(int max_new, const StrataSampling & s, const std::vector<int> & ids) {
    std::string out = "GEN " + std::to_string(std::max(1, max_new));
    if (s.temp > 0) out += " temperature=" + fmt_g(s.temp);
    if (s.top_p < 1.0f) out += " top_p=" + fmt_g(s.top_p);
    if (s.top_k >= 0) out += " top_k=" + std::to_string(s.top_k >= 1 && s.top_k <= 64 ? s.top_k : 64);
    if (s.min_p > 0 && s.min_p <= 1.0f) out += " min_p=" + fmt_g(s.min_p);
    if (s.seed > 0) out += " seed=" + std::to_string(s.seed);
    out += ' ';
    for (size_t i = 0; i < ids.size(); i++) {
        if (i) out += ',';
        out += std::to_string(ids[i]);
    }
    return out;
}

std::string strata_second_shard(const std::string & path) {
    static const std::string tag = "-of-";
    size_t of = path.rfind(tag);
    if (of == std::string::npos || path.compare(path.size() >= 5 ? path.size() - 5 : 0, 5, ".gguf") != 0) return "";
    if (of < 6 || path.compare(of - 6, 1, "-") != 0) return "";
    std::string k = path.substr(of - 5, 5), n = path.substr(of + 4, path.size() - (of + 4) - 5);
    if (n != "00002" || k != "00001") return "";
    for (char c : k + n)
        if (c < '0' || c > '9') return "";
    return path.substr(0, of - 5) + "00002-of-00002.gguf";
}

std::string strata_chat_template_path(const ModelConfig & m) {  // override file, else the pack's copy
    if (!m.chat_template_file.empty()) return home_expand(m.chat_template_file);
    if (m.strata_pack.empty()) return "";
    return home_expand(m.strata_pack) + "/tokenizer/chat_template.jinja";
}

std::vector<std::string> strata_argv(const ModelConfig & m, int max_context, const std::string & profile) {
    auto need = [&](const std::string & v, const char * key) {
        if (v.empty()) throw std::runtime_error(std::string("strata backend: models.<name>.") + key + " is not set");
        return home_expand(v);
    };
    std::vector<std::string> a;
    a.push_back("--serve");
    a.push_back("--pack");
    a.push_back(need(m.strata_pack, "strata_pack"));
    std::string first = need(m.path, "path");
    a.push_back("--native");
    a.push_back(first);
    std::string second = strata_second_shard(first);
    if (!second.empty()) {
        a.push_back("--ple-gguf");
        a.push_back(second);
    }
    a.push_back("--expert-profile");
    a.push_back(profile);
    a.push_back("--expert-cache");
    a.push_back("auto");
    a.push_back("--prefill");
    a.push_back("auto");
    a.push_back("--spec");
    a.push_back(std::to_string(m.strata_spec));
    a.push_back("--spec-min-p");
    a.push_back(fmt_g(m.strata_spec_min_p));
    a.push_back("--mtp");
    a.push_back(need(m.strata_mtp, "strata_mtp"));
    a.push_back("--max-context");
    a.push_back(std::to_string(max_context));
    a.push_back("--kv");
    a.push_back(m.strata_kv);
    a.push_back("--kv-resident");
    a.push_back(std::to_string(m.strata_kv_resident));
    a.push_back("--vram-reserve-mib");
    a.push_back(std::to_string(m.strata_vram_reserve_mib));
    if (m.strata_resident_budget_gib > 0) {
        a.push_back("--mmap-experts");
        a.push_back("--resident-budget-gib");
        a.push_back(std::to_string(m.strata_resident_budget_gib));
    }
    for (const auto & x : m.strata_extra_args) a.push_back(x);
    return a;
}

StrataLine parse_strata_line(const std::string & raw) {
    StrataLine l;
    std::string line = raw;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    auto f = split_ws(line);
    if (f.empty()) return l;
    const std::string & k = f[0];
    if (k == "T" && f.size() >= 2 && is_int(f[1])) {
        l.kind = StrataLine::Token;
        l.token = std::atoi(f[1].c_str());
    } else if (k == "PP" && f.size() >= 3 && is_int(f[1]) && is_int(f[2])) {
        l.kind = StrataLine::Prompt;
        l.pos = std::atoi(f[1].c_str());
        l.total = std::atoi(f[2].c_str());
        if (f.size() >= 5) l.tok_s = std::atof(f[4].c_str());
    } else if ((k == "RESUME" || k == "REUSED") && f.size() >= 2 && is_int(f[1])) {
        l.kind = k == "RESUME" ? StrataLine::Resume : StrataLine::Reused;
        l.reused = std::atoi(f[1].c_str());
    } else if (k == "DONE" && f.size() >= 6) {
        l.kind = StrataLine::Done;
        l.generated = std::atoi(f[1].c_str());
        l.prompt_tokens = std::atoi(f[2].c_str());
        l.prompt_ms = std::atof(f[3].c_str());
        l.decode_ms = std::atof(f[4].c_str());
        l.finish = f[5];
        if (f.size() >= 8) {
            l.drafts_accepted = std::atoi(f[6].c_str());
            l.drafts_offered = std::atoi(f[7].c_str());
        }
        if (f.size() >= 15) l.prompt_read = std::atoi(f[14].c_str());
    } else if (k == "ERR") {
        l.kind = StrataLine::Err;
        l.text = line.size() > 4 ? line.substr(4) : "";
    } else if (k == "READY" && f.size() >= 2 && is_int(f[1])) {
        l.kind = StrataLine::Ready;
        l.max_context = std::atoi(f[1].c_str());
        l.text = line;
        l.can_stop = std::find(f.begin() + 2, f.end(), "stop") != f.end();
    } else if (k == "INFO") {
        l.kind = StrataLine::Info;
        l.text = line;
    }
    return l;
}

std::string strata_finish_reason(const std::string & f) {
    if (f == "cancel") return "cancelled";
    if (f == "length") return "length";
    return "stop";
}

size_t utf8_complete_prefix(const std::string & s) {
    size_t n = s.size();
    // look back at most 3 bytes for the start of a sequence that is not finished
    for (size_t back = 1; back <= 3 && back <= n; back++) {
        unsigned char c = (unsigned char) s[n - back];
        if ((c & 0xC0) == 0x80) continue;           // continuation byte: keep looking for its lead byte
        size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        return need > back ? n - back : n;
    }
    return n;
}

}  // namespace judas
