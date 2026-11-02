#include "tools/swarm_tools.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include "core/cpu_topology.h"
#include "core/json.h"
#include "cpp-httplib/httplib.h"

namespace judas {
namespace fs = std::filesystem;

std::vector<std::string> swarm_chunks(const std::string & text, size_t max_chars) {
    std::vector<std::string> out;
    if (text.empty()) return out;
    if (max_chars < 1) max_chars = 1;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = std::min(text.size(), pos + max_chars);
        if (end < text.size()) {
            // prefer a line break in the last quarter of the piece
            size_t lo = pos + max_chars - max_chars / 4;
            size_t nl = text.rfind('\n', end - 1);
            if (nl != std::string::npos && nl >= lo && nl > pos) end = nl + 1;
            else while (end > pos + 1 && ((unsigned char) text[end] & 0xC0) == 0x80) end--;
        }
        out.push_back(text.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

static std::string glob_regex(const std::string & g) {
    std::string r = "^";
    int brace = 0;
    for (size_t i = 0; i < g.size(); i++) {
        char c = g[i];
        if (c == '*') {
            if (i + 1 < g.size() && g[i + 1] == '*') {
                i++;
                if (i + 1 < g.size() && g[i + 1] == '/') { i++; r += "(?:.*/)?"; }
                else r += ".*";
            } else {
                r += "[^/]*";
            }
        } else if (c == '?') {
            r += "[^/]";
        } else if (c == '{') {
            brace++;
            r += "(?:";
        } else if (c == '}' && brace > 0) {
            brace--;
            r += ")";
        } else if (c == ',' && brace > 0) {
            r += "|";
        } else if (std::string(".+()|^$\\[]").find(c) != std::string::npos) {
            r += '\\';
            r += c;
        } else {
            r += c;
        }
    }
    return r + "$";
}

bool swarm_glob_match(const std::string & pattern, const std::string & path) {
    try {
        return std::regex_match(path, std::regex(glob_regex(pattern)));
    } catch (...) {
        return false;
    }
}

std::vector<SwarmReply> swarm_run(size_t n, int parallel, const std::function<SwarmReply(size_t)> & fn,
                                  const std::function<void(size_t, size_t)> & progress) {
    std::vector<SwarmReply> res(n);
    std::atomic<size_t> next{0};
    size_t done = 0;
    std::mutex mu;
    auto worker = [&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= n) return;
            SwarmReply r;
            try {
                r = fn(i);
            } catch (const std::exception & e) {
                r = {false, e.what()};
            }
            std::lock_guard lk(mu);
            res[i] = std::move(r);
            done++;
            if (progress) progress(done, n);
        }
    };
    int t = std::max(1, std::min<int>(parallel, (int) std::max<size_t>(n, 1)));
    std::vector<std::thread> ths;
    for (int i = 0; i < t; i++) ths.emplace_back(worker);
    for (auto & th : ths) th.join();
    return res;
}

SwarmReply swarm_complete(const std::string & url, const std::string & instruction, const std::string & item) {
    std::string base = url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    httplib::Client cli(base);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(900);
    cli.set_write_timeout(30);
    json body = {{"model", "swarm"},
                 {"stream", false},
                 {"max_tokens", 1500},
                 {"messages", json::array({{{"role", "system"}, {"content", instruction}},
                                           {{"role", "user"}, {"content", item}}})}};
    auto r = cli.Post("/v1/chat/completions", body.dump(), "application/json");
    if (!r) return {false, "swarm unreachable at " + base + " (" + httplib::to_string(r.error()) +
                               "); start it with: systemctl --user start judas-swarm"};
    if (r->status != 200) return {false, "swarm HTTP " + std::to_string(r->status) + ": " + r->body.substr(0, 300)};
    try {
        json j = json::parse(r->body);
        const json & m = j.at("choices").at(0).at("message");
        std::string t = m.contains("content") && m["content"].is_string() ? m["content"].get<std::string>() : "";
        return {true, t};
    } catch (const std::exception & e) {
        return {false, std::string("bad swarm reply: ") + e.what()};
    }
}

namespace {

struct Item {
    std::string id;       // "src/a.cpp" or "src/a.cpp#2/3" or "text:1"
    std::string content;
    std::string error;    // non-empty: not sent
};

bool has_glob(const std::string & s) { return s.find_first_of("*?{") != std::string::npos; }

bool skip_dir(const std::string & n) {
    return n == ".git" || n == "node_modules" || n == "build" || n == "__pycache__" || n == ".venv" ||
           n == "target" || n == ".cache" || n == ".judas";
}

// Files under cwd matching a relative glob (or absolute pattern), sorted, at most `cap`.
std::vector<std::string> expand_glob(const ToolContext & ctx, const std::string & pat, size_t cap) {
    std::vector<std::string> out;
    fs::path root = ctx.cwd.empty() ? fs::path(".") : fs::path(ctx.cwd);
    bool abs = !pat.empty() && pat[0] == '/';
    if (abs) root = "/";
    // start the walk at the longest literal directory prefix
    std::string prefix;
    {
        size_t cut = pat.find_first_of("*?{");
        std::string lit = pat.substr(0, cut);
        size_t sl = lit.rfind('/');
        if (sl != std::string::npos) prefix = lit.substr(0, sl);
    }
    fs::path start = abs ? fs::path(prefix.empty() ? "/" : prefix) : (prefix.empty() ? root : root / prefix);
    std::error_code ec;
    if (!fs::is_directory(start, ec)) return out;
    fs::recursive_directory_iterator it(start, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec)) {
            if (skip_dir(it->path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        std::string rel = abs ? it->path().string() : fs::relative(it->path(), root, ec).string();
        if (swarm_glob_match(pat, rel)) {
            out.push_back(rel);
            if (out.size() >= cap) break;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool looks_binary(const std::string & s) {
    return memchr(s.data(), 0, std::min<size_t>(s.size(), 8000)) != nullptr;
}

std::string clip(const std::string & s, size_t n) {
    if (s.size() <= n) return s;
    size_t e = n;
    while (e > 0 && ((unsigned char) s[e] & 0xC0) == 0x80) e--;
    return s.substr(0, e) + "...";
}

std::string stamp() {
    std::time_t t = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", std::localtime(&t));
    return b;
}

ToolResult t_swarm_map(const json & a, ToolContext & ctx) {
    const Config & cfg = *ctx.cfg;
    if (!swarm_is_on(cfg)) return {false, "swarm is off; turn it on in Settings or the Swarm tab (or run `judas swarm on`)"};
    std::string instruction = a.value("instruction", std::string());
    if (instruction.empty()) return {false, "instruction is required"};
    if (!a.contains("items") || !a["items"].is_array() || a["items"].empty())
        return {false, "items must be a non-empty array of file paths, globs, or \"text:...\" strings"};
    const size_t max_items = 2000;
    const size_t max_chars = (size_t) std::max(500, cfg.swarm.max_item_chars);

    std::vector<Item> items;
    std::vector<std::string> notes;
    int text_n = 0;
    for (const auto & it : a["items"]) {
        if (!it.is_string()) continue;
        std::string s = it.get<std::string>();
        if (s.rfind("text:", 0) == 0) {
            std::string body = s.substr(5);
            auto parts = swarm_chunks(body, max_chars);
            ++text_n;
            for (size_t k = 0; k < parts.size(); k++)
                items.push_back({"text:" + std::to_string(text_n) + (parts.size() > 1 ? "#" + std::to_string(k + 1) + "/" + std::to_string(parts.size()) : ""),
                                 parts[k], ""});
            continue;
        }
        std::vector<std::string> paths;
        if (has_glob(s)) {
            paths = expand_glob(ctx, s, max_items);
            if (paths.empty()) notes.push_back("glob matched nothing: " + s);
        } else {
            paths.push_back(s);
        }
        for (const auto & p : paths) {
            std::string resolved;
            std::string deny = check_read_path(ctx, p, resolved);
            if (!deny.empty()) { items.push_back({p, "", deny}); continue; }
            std::error_code ec;
            if (!fs::is_regular_file(resolved, ec)) { items.push_back({p, "", "not a regular file"}); continue; }
            std::ifstream f(resolved, std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            std::string data = ss.str();
            if (looks_binary(data)) { items.push_back({p, "", "binary file skipped"}); continue; }
            if (data.empty()) { items.push_back({p, "", "empty file skipped"}); continue; }
            auto parts = swarm_chunks(data, max_chars);
            for (size_t k = 0; k < parts.size(); k++)
                items.push_back({p + (parts.size() > 1 ? "#" + std::to_string(k + 1) + "/" + std::to_string(parts.size()) : ""),
                                 parts[k], ""});
        }
        if (items.size() > max_items) break;
    }
    if (items.size() > max_items) {
        items.resize(max_items);
        notes.push_back("only the first " + std::to_string(max_items) + " items were run");
    }
    if (items.empty()) return {false, "no items to run" + (notes.empty() ? "" : ": " + notes[0])};

    // indices of items that are actually sent
    std::vector<size_t> run_idx;
    for (size_t i = 0; i < items.size(); i++) if (items[i].error.empty()) run_idx.push_back(i);

    // fail fast with a clear message when the swarm is down
    if (!run_idx.empty()) {
        std::string base = cfg.swarm.url;
        httplib::Client probe(base);
        probe.set_connection_timeout(2);
        probe.set_read_timeout(3);
        auto h = probe.Get("/health");
        if (!h) return {false, "the swarm instance is not reachable at " + base + ". Ask the user to start it: "
                               "systemctl --user start judas-swarm (see docs/SWARM.md)."};
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<SwarmReply> replies = swarm_run(
        run_idx.size(), cfg.swarm.max_parallel,
        [&](size_t k) { return swarm_complete(cfg.swarm.url, instruction, items[run_idx[k]].content); },
        [&](size_t done, size_t total) {
            if (ctx.progress) ctx.progress({{"kind", "swarm"}, {"done", done}, {"total", total}});
        });
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // full results to JSONL
    std::string outp = a.value("output", std::string());
    fs::path op;
    if (outp.empty()) op = fs::path(ctx.cwd.empty() ? "." : ctx.cwd) / ".judas" / "swarm" / (stamp() + ".jsonl");
    else {
        std::string resolved;
        std::string deny = check_write_path(ctx, outp, resolved);
        if (!deny.empty()) return {false, "output path refused: " + deny};
        op = resolved;
    }
    std::string werr;
    {
        std::error_code ec;
        fs::create_directories(op.parent_path(), ec);
    }
    std::vector<json> rows(items.size());
    size_t ok_n = 0, err_n = 0;
    {
        size_t k = 0;
        for (size_t i = 0; i < items.size(); i++) {
            if (!items[i].error.empty()) { rows[i] = {{"id", items[i].id}, {"ok", false}, {"error", items[i].error}}; err_n++; continue; }
            const SwarmReply & r = replies[k++];
            if (r.ok) { rows[i] = {{"id", items[i].id}, {"ok", true}, {"text", r.text}}; ok_n++; }
            else { rows[i] = {{"id", items[i].id}, {"ok", false}, {"error", r.text}}; err_n++; }
        }
        std::ofstream f(op);
        if (!f) werr = "could not write " + op.string();
        else for (auto & row : rows) f << row.dump() << "\n";
    }

    const size_t per = 400, total_budget = 12000;
    std::ostringstream o;
    o << "swarm_map: " << ok_n << " ok, " << err_n << " failed of " << items.size() << " items in "
      << (long) secs << " s. Full results (JSONL: id, ok, text|error): " << op.string() << (werr.empty() ? "" : " (" + werr + ")") << "\n";
    for (auto & n : notes) o << "note: " << n << "\n";
    size_t used = 0;
    size_t shown = 0;
    for (auto & row : rows) {
        std::string line = "- " + row["id"].get<std::string>() + ": ";
        if (row["ok"].get<bool>()) {
            std::string t = row["text"].get<std::string>();
            std::replace(t.begin(), t.end(), '\n', ' ');
            line += clip(t, per);
        } else {
            line += "ERROR " + clip(row["error"].get<std::string>(), per);
        }
        if (used + line.size() > total_budget) break;
        used += line.size();
        shown++;
        o << line << "\n";
    }
    if (shown < rows.size())
        o << "... " << (rows.size() - shown) << " more results are in the JSONL file; read it in parts with read_file.\n";
    return {true, o.str(), false};
}

}  // namespace

void register_swarm_tool(ToolRegistry & reg) {
    Tool t;
    t.name = "swarm_map";
    t.description =
        "Run one instruction over many items on a swarm of small fast models (CPU, in parallel). Use it for research "
        "(pull relevant facts from many saved pages), big documents (auto-chunked, each chunk read in parallel), and bulk "
        "extraction, summaries, triage, classification or tagging over many files or snippets; not for coding or long "
        "reasoning. Each item is answered independently. Returns a compact list; full replies go to a JSONL file.";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"instruction", {{"type", "string"}, {"description", "what to do with each item; becomes the system prompt, so be specific about the output format"}}},
                      {"items", {{"type", "array"}, {"items", {{"type", "string"}}},
                                 {"description", "file paths relative to the working directory, globs (src/**/*.md), or literal text prefixed with text:"}}},
                      {"output", {{"type", "string"}, {"description", "optional path for the JSONL results (default .judas/swarm/<timestamp>.jsonl)"}}}}},
                    {"required", {"instruction", "items"}}};
    t.writes = false;
    t.run = t_swarm_map;
    reg.add(std::move(t));
}

// ---- cores ----------------------------------------------------------------------------------------------------

// Fill threads/pin/cpus from the instance's config file.
static void cores_from_file(const Config & cfg, SwarmCores & out) {
    out.config_path = expand_home(cfg.swarm.config);
    Config sc = load_config_file(out.config_path);
    auto it = sc.models.find(sc.engine.model);
    out.threads = it != sc.models.end() ? it->second.threads : 0;
    out.pin = sc.engine.pin_cores;
    if (out.pin && out.threads > 0) out.cpus = cpu_pin_set(out.threads);
}

SwarmCores swarm_cores_get(const Config & cfg) {
    SwarmCores out;
    out.nproc = cpu_count();
    cores_from_file(cfg, out);
    try {
        httplib::Client cli(cfg.swarm.url);
        cli.set_connection_timeout(1);
        cli.set_read_timeout(2);
        if (auto r = cli.Get("/api/cores"); r && r->status == 200) {
            json j = json::parse(r->body);
            out.up = true;
            out.threads = j.value("threads", out.threads);
            out.nproc = j.value("nproc", out.nproc);
            out.pin = j.value("pinned", out.pin);
            out.cpus = j.value("cpus", out.cpus);
        }
    } catch (...) {}
    out.ok = true;
    return out;
}

SwarmCores swarm_cores_set(const Config & cfg, int n, int pin) {
    SwarmCores out;
    out.nproc = cpu_count();
    cores_from_file(cfg, out);
    if (n < 1 || n > out.nproc) {
        out.error = "cores must be 1.." + std::to_string(out.nproc);
        return out;
    }
    Config sc = load_config_file(out.config_path);
    if (!sc.models.count(sc.engine.model)) {
        out.error = "no [models." + sc.engine.model + "] in " + out.config_path;
        return out;
    }
    bool want_pin = pin < 0 ? sc.engine.pin_cores : pin != 0;
    if (!set_toml_value_in_file(out.config_path, "models." + sc.engine.model, "threads", std::to_string(n)) ||
        !set_toml_value_in_file(out.config_path, "engine", "pin_cores", want_pin ? "true" : "false")) {
        out.error = "could not write " + out.config_path;
        return out;
    }
    out.threads = n;
    out.pin = want_pin;
    out.cpus = want_pin ? cpu_pin_set(n) : std::vector<int>{};
    out.ok = true;
    try {
        httplib::Client cli(cfg.swarm.url);
        cli.set_connection_timeout(1);
        cli.set_read_timeout(5);
        json body = {{"threads", n}, {"pin", want_pin}};
        if (auto r = cli.Post("/api/cores", body.dump(), "application/json"); r && r->status == 200) {
            json j = json::parse(r->body);
            out.up = true;
            out.live = j.value("ok", false);
            out.cpus = j.value("cpus", out.cpus);
        }
    } catch (...) {}
    return out;
}

// ---- power ----------------------------------------------------------------------------------------------------

namespace {
std::atomic<int> g_swarm_override{-1};  // -1: follow the config, 0 off, 1 on
}

bool swarm_is_on(const Config & cfg) {
    int o = g_swarm_override.load();
    return o < 0 ? cfg.swarm.enabled : o != 0;
}

void swarm_set_on(bool on) { g_swarm_override = on ? 1 : 0; }

bool swarm_write_enabled(const std::string & config_path, bool on) {
    return set_toml_value_in_file(config_path, "swarm", "enabled", on ? "true" : "false");
}

SwarmPower swarm_power(const Config & cfg, bool on) {
    SwarmPower out;
    out.enabled = swarm_is_on(cfg);
    if (!valid_unit_name(cfg.swarm.unit)) {
        out.error = "[swarm].unit is not a valid systemd unit name: " + cfg.swarm.unit;
        return out;
    }
    std::string path = user_config_path();
    if (!swarm_write_enabled(path, on)) {
        out.error = "could not write " + path;
        return out;
    }
    swarm_set_on(on);
    out.enabled = on;
    out.ok = true;
    std::string cmd = std::string("systemctl --user ") + (on ? "start " : "stop ") + cfg.swarm.unit + " >/dev/null 2>&1";
    if (std::system(cmd.c_str()) != 0 && on)
        out.error = "could not start " + cfg.swarm.unit + "; check `systemctl --user status " + cfg.swarm.unit + "`";
    return out;
}

bool swarm_unit_active(const Config & cfg) {
    if (!valid_unit_name(cfg.swarm.unit)) return false;
    std::string cmd = "systemctl --user is-active --quiet " + cfg.swarm.unit + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

}  // namespace judas
