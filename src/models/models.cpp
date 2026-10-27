// Model discovery, fit checks, Hugging Face search/download and config writing for `judas models`.

#include "models/models.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include "core/json.h"
#include "cpp-httplib/httplib.h"
#include "gguf.h"

namespace judas {
const char * version();
namespace fs = std::filesystem;

namespace {

constexpr uint64_t GiB = 1024ull * 1024 * 1024;
constexpr uint64_t OVERHEAD = GiB + GiB / 4;   // compute buffers and the runtime, roughly, on top of the weights

std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

bool ends_with(const std::string & s, const std::string & suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

const std::regex & split_re() {
    static const std::regex re(R"(^(.*)-(\d{5})-of-(\d{5})\.gguf$)", std::regex::icase);
    return re;
}

// llama_ftype -> the name people know it by
std::string ftype_name(uint32_t t) {
    static const std::map<uint32_t, const char *> names = {
        {0, "F32"},      {1, "F16"},      {2, "Q4_0"},     {3, "Q4_1"},    {7, "Q8_0"},    {8, "Q5_0"},
        {9, "Q5_1"},     {10, "Q2_K"},    {11, "Q3_K_S"},  {12, "Q3_K_M"}, {13, "Q3_K_L"}, {14, "Q4_K_S"},
        {15, "Q4_K_M"},  {16, "Q5_K_S"},  {17, "Q5_K_M"},  {18, "Q6_K"},   {19, "IQ2_XXS"}, {20, "IQ2_XS"},
        {21, "Q2_K_S"},  {22, "IQ3_XS"},  {23, "IQ3_XXS"}, {24, "IQ1_S"},  {25, "IQ4_NL"}, {26, "IQ3_S"},
        {27, "IQ3_M"},   {28, "IQ2_S"},   {29, "IQ2_M"},   {30, "IQ4_XS"}, {31, "IQ1_M"},  {32, "BF16"},
        {36, "TQ1_0"},   {37, "TQ2_0"},   {38, "MXFP4"},
    };
    auto it = names.find(t);
    return it == names.end() ? "" : it->second;
}

// The quant from a file name when the header has none ("...-Q4_K_M.gguf", "...UD-Q4_K_XL...").
std::string quant_from_name(const std::string & file) {
    static const std::regex re(R"((?:^|[-_.])((?:UD-)?(?:I?Q\d(?:_[A-Z0-9]+)*|F16|BF16|F32|MXFP4))(?:[-_.]|$))",
                               std::regex::icase);
    std::smatch m;
    std::string base = fs::path(file).stem().string();
    std::string found;
    for (auto it = std::sregex_iterator(base.begin(), base.end(), re); it != std::sregex_iterator(); ++it)
        found = (*it)[1].str();
    for (auto & c : found) c = (char) std::toupper((unsigned char) c);
    return found;
}

uint64_t kv_uint(const gguf_context * g, const std::string & key) {
    int64_t id = gguf_find_key(g, key.c_str());
    if (id < 0) return 0;
    switch (gguf_get_kv_type(g, id)) {
        case GGUF_TYPE_UINT32: return gguf_get_val_u32(g, id);
        case GGUF_TYPE_INT32: return (uint64_t) std::max(0, gguf_get_val_i32(g, id));
        case GGUF_TYPE_UINT64: return gguf_get_val_u64(g, id);
        case GGUF_TYPE_INT64: return (uint64_t) std::max<int64_t>(0, gguf_get_val_i64(g, id));
        case GGUF_TYPE_UINT16: return gguf_get_val_u16(g, id);
        default: return 0;
    }
}

std::string kv_str(const gguf_context * g, const std::string & key) {
    int64_t id = gguf_find_key(g, key.c_str());
    if (id < 0 || gguf_get_kv_type(g, id) != GGUF_TYPE_STRING) return "";
    return gguf_get_val_str(g, id);
}

std::string canon(const std::string & p) {
    std::error_code ec;
    auto c = fs::canonical(p, ec);
    return ec ? p : c.string();
}

// Sum the sizes of every part of a split model; path is part 1.
void count_parts(GgufInfo & m) {
    std::smatch sm;
    std::string file = fs::path(m.path).filename().string();
    if (!std::regex_match(file, sm, split_re())) return;
    int total = std::stoi(sm[3].str());
    std::error_code ec;
    uint64_t sum = 0;
    int found = 0;
    for (int i = 1; i <= total; i++) {
        char part[64];
        std::snprintf(part, sizeof part, "-%05d-of-%05d.gguf", i, total);
        fs::path p = fs::path(m.path).parent_path() / (sm[1].str() + part);
        auto sz = fs::file_size(p, ec);
        if (!ec) {
            sum += sz;
            found++;
        }
        ec.clear();
    }
    m.parts = total;
    m.bytes = sum;
    if (found != total) m.error = "only " + std::to_string(found) + " of " + std::to_string(total) + " parts present";
}

void add_file(std::vector<GgufInfo> & out, std::set<std::string> & seen, const std::string & path,
              const std::string & source, const std::string & label = "") {
    std::smatch sm;
    std::string file = fs::path(path).filename().string();
    if (std::regex_match(file, sm, split_re()) && sm[2].str() != "00001") return;   // later parts
    std::string c = canon(path);
    if (!seen.insert(c).second) return;
    GgufInfo m = gguf_info(path);
    m.source = source;
    if (!label.empty()) m.label = label;
    out.push_back(std::move(m));
}

void scan_dir(std::vector<GgufInfo> & out, std::set<std::string> & seen, const std::string & dir,
              const std::string & source, int max_depth) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const auto & e = *it;
        std::string name = e.path().filename().string();
        if (e.is_directory(ec)) {
            if (it.depth() >= max_depth || name == ".git" || name == "node_modules" || name == "blobs")
                it.disable_recursion_pending();
            continue;
        }
        if (!ends_with(lower(name), ".gguf")) continue;
        if (!e.is_regular_file(ec)) continue;   // follows symlinks (Hugging Face snapshots are links to blobs)
        add_file(out, seen, e.path().string(), source);
    }
}

// Ollama keeps GGUFs as content-addressed blobs; the manifests say which blob is the model.
void scan_ollama(std::vector<GgufInfo> & out, std::set<std::string> & seen, const std::string & root) {
    std::error_code ec;
    fs::path manifests = fs::path(root) / "manifests";
    if (!fs::is_directory(manifests, ec)) return;
    for (fs::recursive_directory_iterator it(manifests, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        try {
            std::ifstream in(it->path());
            json j = json::parse(in);
            for (auto & layer : j.value("layers", json::array())) {
                if (layer.value("mediaType", "") != "application/vnd.ollama.image.model") continue;
                std::string digest = layer.value("digest", "");
                std::replace(digest.begin(), digest.end(), ':', '-');
                fs::path blob = fs::path(root) / "blobs" / digest;
                // manifests/<registry>/<namespace>/<name>/<tag>
                fs::path rel = fs::relative(it->path(), manifests, ec);
                std::vector<std::string> parts;
                for (auto & p : rel) parts.push_back(p.string());
                std::string label = "ollama:";
                if (parts.size() >= 4) {
                    if (parts[1] != "library") label += parts[1] + "/";
                    label += parts[2] + ":" + parts[3];
                } else {
                    label += rel.string();
                }
                if (fs::is_regular_file(blob, ec)) add_file(out, seen, blob.string(), "ollama", label);
            }
        } catch (...) {
        }
    }
}

std::string url_encode(const std::string & s, bool keep_slash) {
    std::string o;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keep_slash && c == '/')) {
            o += (char) c;
        } else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            o += buf;
        }
    }
    return o;
}

httplib::Headers hf_headers() {
    httplib::Headers h = {{"User-Agent", std::string("judas/") + version()}};
    if (const char * t = std::getenv("HF_TOKEN"); t && *t) h.emplace("Authorization", std::string("Bearer ") + t);
    return h;
}

std::unique_ptr<httplib::Client> hf_client() {
    auto c = std::make_unique<httplib::Client>("https://huggingface.co");
    c->set_follow_location(true);
    c->set_connection_timeout(15);
    c->set_read_timeout(60);
    return c;
}

}  // namespace

std::string human_bytes(uint64_t b) {
    char buf[32];
    if (b >= GiB) std::snprintf(buf, sizeof buf, "%.1f GB", b / (double) GiB);
    else if (b >= 1024 * 1024) std::snprintf(buf, sizeof buf, "%.0f MB", b / 1048576.0);
    else std::snprintf(buf, sizeof buf, "%llu KB", (unsigned long long) (b / 1024));
    return buf;
}

GgufInfo gguf_info(const std::string & path) {
    GgufInfo m;
    m.path = path;
    m.label = fs::path(path).filename().string();
    std::error_code ec;
    m.bytes = fs::file_size(path, ec);
    if (ec) {
        m.error = "file missing";
        return m;
    }
    count_parts(m);
    gguf_init_params params{true, nullptr};
    gguf_context * g = gguf_init_from_file(path.c_str(), params);
    if (!g) {
        m.error = "not a readable GGUF file";
        return m;
    }
    m.ok = true;
    m.arch = kv_str(g, "general.architecture");
    m.name = kv_str(g, "general.name");
    m.size_label = kv_str(g, "general.size_label");
    if (gguf_find_key(g, "general.file_type") >= 0) m.quant = ftype_name((uint32_t) kv_uint(g, "general.file_type"));
    if (m.quant.empty()) m.quant = quant_from_name(path);
    if (!m.arch.empty()) {
        m.ctx_train = (uint32_t) kv_uint(g, m.arch + ".context_length");
        m.experts = (uint32_t) kv_uint(g, m.arch + ".expert_count");
        m.experts_used = (uint32_t) kv_uint(g, m.arch + ".expert_used_count");
    }
    m.projector = m.arch == "clip" || lower(m.label).find("mmproj") != std::string::npos;
    {
        std::string l = lower(m.label);
        m.helper = l.find("embed") != std::string::npos || l.find("rerank") != std::string::npos ||
                   (m.arch.size() > 4 && m.arch.compare(m.arch.size() - 4, 4, "-mtp") == 0);
    }
    gguf_free(g);
    return m;
}

std::vector<std::pair<std::string, std::string>> model_search_dirs(const Config & cfg) {
    std::vector<std::pair<std::string, std::string>> d;
    if (const char * e = std::getenv("JUDAS_MODEL_DIRS"); e && *e) {
        std::stringstream ss(e);
        std::string part;
        while (std::getline(ss, part, ':'))
            if (!part.empty()) d.push_back({expand_home(part), "dir"});
    }
    for (auto & [name, m] : cfg.models)
        if (!m.path.empty()) d.push_back({fs::path(expand_home(m.path)).parent_path().string(), "dir"});
    d.push_back({expand_home("~/models"), "dir"});
    d.push_back({expand_home("~/.lmstudio/models"), "lm-studio"});
    d.push_back({expand_home("~/.cache/lm-studio/models"), "lm-studio"});
    std::string hf = expand_home("~/.cache/huggingface/hub");
    if (const char * e = std::getenv("HF_HUB_CACHE"); e && *e) hf = expand_home(e);
    else if (const char * h = std::getenv("HF_HOME"); h && *h) hf = (fs::path(expand_home(h)) / "hub").string();
    d.push_back({hf, "huggingface"});
    d.push_back({expand_home("~/.cache/llama.cpp"), "llama.cpp"});
    d.push_back({expand_home("~/.local/share/nomic.ai/GPT4All"), "gpt4all"});
    d.push_back({expand_home("~/Downloads"), "downloads"});
    return d;
}

std::vector<GgufInfo> scan_models(const Config & cfg, const std::vector<std::string> & extra_dirs) {
    std::vector<GgufInfo> out;
    std::set<std::string> seen;
    // configured models first, so their names show even when the file sits somewhere unusual
    for (auto & [name, mc] : cfg.models) {
        if (mc.path.empty()) continue;
        std::string p = expand_home(mc.path);
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            GgufInfo m;
            m.path = p;
            m.label = fs::path(p).filename().string();
            m.source = "config";
            m.error = "file missing";
            m.config_name = name;
            out.push_back(m);
            continue;
        }
        add_file(out, seen, p, "config");
    }
    for (auto & d : extra_dirs) scan_dir(out, seen, expand_home(d), "dir", 6);
    for (auto & [dir, source] : model_search_dirs(cfg))
        scan_dir(out, seen, dir, source, source == "downloads" ? 2 : 6);
    std::string ollama = expand_home("~/.ollama/models");
    if (const char * e = std::getenv("OLLAMA_MODELS"); e && *e) ollama = expand_home(e);
    scan_ollama(out, seen, ollama);
    scan_ollama(out, seen, "/usr/share/ollama/.ollama/models");

    for (auto & m : out) {
        if (!m.config_name.empty()) continue;
        std::string c = canon(m.path);
        for (auto & [name, mc] : cfg.models)
            if (!mc.path.empty() && canon(expand_home(mc.path)) == c) m.config_name = name;
    }
    return out;
}

uint64_t gpu_vram_total() {
    uint64_t best = 0;
    for (int card = 0; card < 8; card++) {
        std::ifstream f("/sys/class/drm/card" + std::to_string(card) + "/device/mem_info_vram_total");
        uint64_t v = 0;
        if (f >> v) best = std::max(best, v);
    }
    if (best) return best;
    if (FILE * p = ::popen("nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits 2>/dev/null", "r")) {
        char line[128];
        while (std::fgets(line, sizeof line, p)) best = std::max<uint64_t>(best, std::strtoull(line, nullptr, 10) * 1024 * 1024);
        ::pclose(p);
    }
    return best;
}

std::string fit_verdict(uint64_t model_bytes, uint64_t vram) {
    if (!vram || !model_bytes) return "?";
    uint64_t need = model_bytes + OVERHEAD + GiB / 2;   // plus a small KV pool
    if (need <= vram * 9 / 10) return "fits";
    if (need <= vram) return "tight";
    return "too big";
}

std::string gguf_display_name(const std::string & path_or_label) {
    std::string n = fs::path(path_or_label).filename().string();
    if (n.size() > 5 && lower(n.substr(n.size() - 5)) == ".gguf") n.resize(n.size() - 5);
    static const std::regex split(R"(-\d{5}-of-\d{5}$)");
    return std::regex_replace(n, split, "");
}

std::string suggest_model_name(const std::string & file_name) {
    std::string s = fs::path(file_name).filename().string();
    std::smatch sm;
    if (std::regex_match(s, sm, split_re())) s = sm[1].str();
    if (ends_with(lower(s), ".gguf")) s = s.substr(0, s.size() - 5);
    if (s.rfind("ollama:", 0) == 0) s = s.substr(7);
    std::string q = quant_from_name(s + ".gguf");
    if (!q.empty()) {
        auto pos = lower(s).rfind(lower(q));
        if (pos != std::string::npos) s = s.substr(0, pos);
    }
    std::string o;
    for (char c : lower(s)) {
        if (std::isalnum((unsigned char) c)) o += c;
        else if (!o.empty() && o.back() != '-') o += '-';
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    if (o.size() > 40) {
        o.resize(40);
        auto cut = o.rfind('-');
        if (cut != std::string::npos && cut >= 20) o.resize(cut);   // end on a whole word
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    return o.empty() ? "model" : o;
}

std::string model_block(const std::string & name, const GgufInfo & m, uint64_t vram, bool cpu) {
    std::string fit = cpu ? "fits" : fit_verdict(m.bytes, vram);
    uint64_t head = vram > m.bytes + OVERHEAD ? vram - m.bytes - OVERHEAD : 0;
    int ctx_total = cpu ? 32768 : head >= 6 * GiB ? 65536 : head >= 3 * GiB ? 32768 : 16384;
    int per = ctx_total / 2;
    if (m.ctx_train && (uint32_t) per > m.ctx_train) per = (int) m.ctx_train;
    if (per < 4096) per = std::min(ctx_total, 4096);
    std::ostringstream o;
    o << "\n[models." << name << "]";
    if (!m.name.empty()) o << "            # " << m.name;
    o << "\npath = \"" << m.path << "\"\n";
    o << (cpu ? "gpu_layers = 0                # CPU only\n" : "gpu_layers = 99\n");
    if (fit == "too big" && m.experts) {
        o << "cpu_moe = true               # too big for " << human_bytes(vram) << " of VRAM: experts stay in RAM\n";
    } else if (fit == "too big") {
        o << "# too big for " << human_bytes(vram) << " of VRAM: lower gpu_layers until it loads (slow), or pick a smaller quant\n";
    }
    o << "ctx_total = " << ctx_total << "            # KV pool shared by all agents\n";
    o << "ctx_per_agent = " << per << "\n";
    o << "kv_type = \"q8_0\"\n";
    o << "flash_attn = true\n";
    o << "sampling = { temp = 0.7, top_p = 0.95 }\n";
    return o.str();
}

std::string add_model_to_config(const std::string & config_path, const std::string & name, const GgufInfo & m,
                                uint64_t vram, bool cpu) {
    static const std::regex ok_name(R"(^[A-Za-z0-9_-]{1,64}$)");
    if (!std::regex_match(name, ok_name)) return "name must be letters, digits, - or _ (got '" + name + "')";
    if (m.projector) return "that file is a vision projector (mmproj), not a model";
    std::error_code ec;
    std::vector<std::string> lines;
    if (fs::exists(config_path, ec)) {
        std::ifstream in(config_path);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    } else {
        std::istringstream in(default_config_toml());
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    }
    // An existing block of that name is replaced only when its file does not exist (the starter placeholder).
    const std::string hdr = "[models." + name + "]";
    for (size_t i = 0; i < lines.size(); i++) {
        std::string t = lines[i].substr(0, lines[i].find('#'));
        while (!t.empty() && std::isspace((unsigned char) t.back())) t.pop_back();
        if (t != hdr) continue;
        size_t j = i + 1;
        std::string old_path;
        for (; j < lines.size(); j++) {
            std::string s = lines[j];
            size_t a = s.find_first_not_of(" \t");
            // the block ends at the next table that is not one of its own sub-tables ([models.<name>.sampling])
            if (a != std::string::npos && s[a] == '[' && s.compare(a, hdr.size(), "[models." + name + ".") != 0) break;
            static const std::regex pre(R"(^\s*path\s*=\s*"([^"]*)\")");
            std::smatch sm;
            if (std::regex_search(s, sm, pre)) old_path = sm[1].str();
        }
        if (!old_path.empty() && fs::exists(expand_home(old_path), ec))
            return "[models." + name + "] already exists and points at " + old_path + "; pick another --name";
        lines.erase(lines.begin() + (long) i, lines.begin() + (long) j);
        break;
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string text;
    for (auto & l : lines) text += l + "\n";
    text += model_block(name, m, vram, cpu);

    fs::create_directories(fs::path(config_path).parent_path(), ec);
    std::string tmp = config_path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << text;
        out.flush();
        if (!out) {
            fs::remove(tmp, ec);
            return "cannot write " + tmp;
        }
    }
    if (fs::exists(config_path, ec)) fs::permissions(tmp, fs::status(config_path, ec).permissions(), ec);
    fs::rename(tmp, config_path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return "cannot replace " + config_path + ": " + ec.message();
    }
    return "";
}

std::vector<HfRepo> hf_search(const std::string & query, int limit, std::string * err) {
    std::vector<HfRepo> out;
    auto cli = hf_client();
    std::string path = "/api/models?filter=gguf&sort=downloads&direction=-1&limit=" + std::to_string(limit) +
                       "&search=" + url_encode(query, false);
    auto r = cli->Get(path, hf_headers());
    if (!r || r->status != 200) {
        if (err) *err = r ? "Hugging Face returned HTTP " + std::to_string(r->status) : "cannot reach huggingface.co (" + httplib::to_string(r.error()) + ")";
        return out;
    }
    try {
        for (auto & j : json::parse(r->body))
            out.push_back({j.value("id", ""), j.value("downloads", 0LL), j.value("likes", 0LL)});
    } catch (const std::exception & e) {
        if (err) *err = std::string("bad reply from Hugging Face: ") + e.what();
    }
    return out;
}

std::vector<HfFile> hf_gguf_files(const std::string & repo, std::string * err) {
    std::vector<HfFile> out;
    auto cli = hf_client();
    auto r = cli->Get("/api/models/" + url_encode(repo, true) + "/tree/main?recursive=true", hf_headers());
    if (!r || r->status != 200) {
        if (err) {
            if (!r) *err = "cannot reach huggingface.co (" + httplib::to_string(r.error()) + ")";
            else if (r->status == 401 || r->status == 403) *err = "no repo " + repo + " (or it is gated/private: set HF_TOKEN)";
            else if (r->status == 404) *err = "no such repo: " + repo;
            else *err = "Hugging Face returned HTTP " + std::to_string(r->status);
        }
        return out;
    }
    try {
        for (auto & j : json::parse(r->body)) {
            std::string p = j.value("path", "");
            if (j.value("type", "") != "file" || !ends_with(lower(p), ".gguf")) continue;
            uint64_t sz = j.contains("lfs") && j["lfs"].is_object() ? j["lfs"].value("size", 0ULL) : j.value("size", 0ULL);
            out.push_back({p, sz});
        }
    } catch (const std::exception & e) {
        if (err) *err = std::string("bad reply from Hugging Face: ") + e.what();
    }
    std::sort(out.begin(), out.end(), [](const HfFile & a, const HfFile & b) { return a.path < b.path; });
    return out;
}

std::string hf_download(const std::string & repo, const std::string & file, const std::string & dir,
                        const std::function<void(uint64_t, uint64_t)> & progress, std::string * err) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path dest = fs::path(dir) / fs::path(file).filename();
    fs::path part = dest.string() + ".part";
    if (fs::exists(dest, ec)) return dest.string();   // already there

    uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;
    auto cli = hf_client();
    cli->set_read_timeout(120);
    auto headers = hf_headers();
    if (have) headers.emplace("Range", "bytes=" + std::to_string(have) + "-");
    std::ofstream out;
    uint64_t total = 0;
    int status = 0;
    std::string path = "/" + url_encode(repo, true) + "/resolve/main/" + url_encode(file, true);
    auto r = cli->Get(
        path, headers,
        [&](const httplib::Response & resp) {
            status = resp.status;
            if (status == 200) have = 0;   // server ignored the range: start over
            else if (status != 206) return false;
            uint64_t len = resp.has_header("Content-Length") ? std::stoull(resp.get_header_value("Content-Length")) : 0;
            total = len ? have + len : 0;
            out.open(part, have ? std::ios::app | std::ios::binary : std::ios::trunc | std::ios::binary);
            return static_cast<bool>(out);
        },
        [&](const char * data, size_t n) {
            out.write(data, (std::streamsize) n);
            have += n;
            if (progress) progress(have, total);
            return static_cast<bool>(out);
        });
    out.close();
    if (!r || (status != 200 && status != 206)) {
        if (err) {
            if (status == 401 || status == 403) *err = "gated or private file: accept the license on huggingface.co and set HF_TOKEN";
            else if (status == 404) *err = "no such file: " + repo + "/" + file;
            else if (status == 416) *err = "partial download is larger than the file; delete " + part.string();
            else if (status) *err = "HTTP " + std::to_string(status);
            else *err = "download failed (" + (r ? std::string("write error") : httplib::to_string(r.error())) + "); run it again to resume";
        }
        return "";
    }
    if (total && fs::file_size(part, ec) != total) {
        if (err) *err = "download stopped early; run it again to resume";
        return "";
    }
    fs::rename(part, dest, ec);
    if (ec) {
        if (err) *err = "cannot rename " + part.string() + ": " + ec.message();
        return "";
    }
    return dest.string();
}

}  // namespace judas
