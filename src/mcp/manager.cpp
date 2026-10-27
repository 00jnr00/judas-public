// See manager.h.
#include "mcp/manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "mcp/launch.h"

namespace judas::mcp {

namespace fs = std::filesystem;

struct Manager::Server {
    McpServerConfig cfg;
    Launch launch;
    std::unique_ptr<Client> client;
    std::thread thr;
    mutable std::mutex mu;            // state, error, names, counts
    std::mutex refresh_mu;
    std::string state = "starting";
    std::string error;
    std::vector<std::string> names;   // registry names currently registered for this server
    int filtered = 0;
};

namespace {

std::string sanitize_name(const std::string & n) {
    std::string o;
    for (char c : n) o += (std::isalnum((unsigned char) c) || c == '_' || c == '-') ? c : '_';
    return o;
}

std::vector<unsigned char> base64_decode(const std::string & in) {
    static int8_t tab[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) tab[i] = -1;
        const char * a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) tab[(unsigned char) a[i]] = (int8_t) i;
        tab[(unsigned char) '-'] = 62;
        tab[(unsigned char) '_'] = 63;
        init = true;
    }
    std::vector<unsigned char> out;
    unsigned v = 0;
    int bits = 0;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (tab[c] < 0) continue;  // whitespace and anything else
        v = (v << 6) | (unsigned) tab[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((unsigned char) ((v >> bits) & 0xFF));
        }
    }
    return out;
}

std::string ext_for(const std::string & mime) {
    static const std::pair<const char *, const char *> m[] = {
        {"image/png", "png"}, {"image/jpeg", "jpg"}, {"image/gif", "gif"}, {"image/webp", "webp"},
        {"image/svg+xml", "svg"}, {"audio/wav", "wav"}, {"audio/x-wav", "wav"}, {"audio/mpeg", "mp3"},
        {"audio/ogg", "ogg"}, {"application/pdf", "pdf"}};
    for (auto & [k, v] : m) if (mime == k) return v;
    auto p = mime.find('/');
    std::string e = p == std::string::npos ? "bin" : sanitize_name(mime.substr(p + 1));
    return e.empty() ? "bin" : e;
}

// Save base64 data under dir; returns the path or "" on failure.
std::string save_blob(const std::string & dir, const std::string & tool, const std::string & b64, const std::string & mime,
                      size_t & bytes) {
    auto data = base64_decode(b64);
    bytes = data.size();
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return "";
    auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string path = dir + "/" + sanitize_name(tool) + "-" + std::to_string(stamp) + "-" + std::to_string(counter++) + "." + ext_for(mime);
    std::ofstream f(path, std::ios::binary);
    if (!f) return "";
    f.write((const char *) data.data(), (std::streamsize) data.size());
    return f ? path : "";
}

bool path_like(const std::string & s) {
    if (s.empty() || s.size() > 4096 || s.find('\n') != std::string::npos) return false;
    return s[0] == '/' || s[0] == '~' || s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0 || s.rfind("file://", 0) == 0;
}

bool command_key(const std::string & k) {
    return k == "command" || k == "cmd" || k == "cmdline" || k == "shell" || k == "script" || k == "argv";
}

// SAFETY FLOOR for MCP arguments. Walks every string: values under command-like keys go through the shell
// tool's command block; anything shaped like a path goes through the secret-path deny list, and (for tools that
// may change things) the write-path policy. Holds in every permission mode.
std::string floor_check(const json & v, const std::string & key, ToolContext & ctx, bool readonly) {
    if (v.is_string()) {
        const std::string & s = v.get_ref<const std::string &>();
        if (command_key(key))
            if (std::string m = safety_floor_command(s); !m.empty()) return m;
        if (path_like(s)) {
            std::string p = s.rfind("file://", 0) == 0 ? s.substr(7) : s;
            if (std::string m = safety_floor_path(ctx, p); !m.empty()) return m;
            if (!readonly && ctx.cfg) {
                std::string resolved;
                if (std::string m = check_write_path(ctx, p, resolved); !m.empty()) return m;
            }
        }
    } else if (v.is_array()) {
        if (command_key(key)) {
            std::string joined;
            for (auto & e : v) if (e.is_string()) joined += (joined.empty() ? "" : " ") + e.get<std::string>();
            if (std::string m = safety_floor_command(joined); !m.empty()) return m;
        }
        for (auto & e : v)
            if (std::string m = floor_check(e, key, ctx, readonly); !m.empty()) return m;
    } else if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it)
            if (std::string m = floor_check(it.value(), it.key(), ctx, readonly); !m.empty()) return m;
    }
    return "";
}

std::string log_name(const McpServerConfig & s) { return "mcp[" + s.name + "]"; }

}  // namespace

std::string trim_description(const std::string & d, size_t max) {
    std::string s;
    bool space = false;
    int newlines = 0;
    for (char c : d) {
        if (c == '\n' && ++newlines >= 2 && !s.empty()) break;  // first paragraph only
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { space = true; continue; }
        newlines = 0;
        if (space && !s.empty()) s += ' ';
        space = false;
        s += c;
    }
    if (s.size() <= max) return s;
    size_t cut = s.rfind(' ', max);
    if (cut == std::string::npos || cut < max / 2) cut = max;
    return s.substr(0, cut) + "...";
}

ToolResult convert_result(const json & result, const std::string & files_dir, const std::string & tool, size_t max_chars) {
    ToolResult r;
    r.ok = !(result.is_object() && result.value("isError", false));
    std::string out;
    auto add = [&](const std::string & piece) {
        if (!out.empty() && out.back() != '\n') out += "\n";
        out += piece;
    };
    bool had_text = false;
    if (result.is_object() && result.contains("content") && result["content"].is_array()) {
        for (auto & b : result["content"]) {
            if (!b.is_object()) continue;
            std::string type = b.value("type", std::string());
            if (type == "text") {
                add(b.value("text", std::string()));
                had_text = true;
            } else if (type == "image" || type == "audio") {
                std::string mime = b.value("mimeType", std::string("application/octet-stream"));
                size_t bytes = 0;
                std::string path = b.contains("data") && b["data"].is_string()
                                       ? save_blob(files_dir, tool, b["data"].get<std::string>(), mime, bytes) : "";
                if (path.empty()) add("[" + type + ": " + mime + ", could not be saved]");
                else add("[" + type + ": " + mime + ", " + std::to_string(bytes) + " bytes, saved to " + path + "]");
            } else if (type == "resource" && b.contains("resource") && b["resource"].is_object()) {
                const json & res = b["resource"];
                std::string uri = res.value("uri", std::string());
                if (res.contains("text") && res["text"].is_string()) {
                    add("[resource " + uri + "]\n" + res["text"].get<std::string>());
                    had_text = true;
                } else if (res.contains("blob") && res["blob"].is_string()) {
                    std::string mime = res.value("mimeType", std::string("application/octet-stream"));
                    size_t bytes = 0;
                    std::string path = save_blob(files_dir, tool, res["blob"].get<std::string>(), mime, bytes);
                    add("[resource " + uri + ": " + mime + ", " + std::to_string(bytes) + " bytes" +
                        (path.empty() ? ", could not be saved]" : ", saved to " + path + "]"));
                }
            } else if (type == "resource_link") {
                add("[resource link: " + b.value("uri", std::string()) +
                    (b.contains("name") && b["name"].is_string() ? " (" + b["name"].get<std::string>() + ")" : "") + "]");
            } else if (!type.empty()) {
                add("[unsupported content block: " + type + "]");
            }
        }
    }
    if (!had_text && result.is_object() && result.contains("structuredContent") && !result["structuredContent"].is_null())
        add(result["structuredContent"].dump(2));
    if (out.empty()) out = r.ok ? "(no output)" : "(the tool reported an error with no message)";
    r.output = truncate_output(out, max_chars);
    return r;
}

// ---------- manager ----------

Manager::Manager(const Config & cfg, ToolRegistry & reg) : cfg_(cfg), reg_(reg) {
    for (const auto & sc : cfg.mcp.servers) {
        auto s = std::make_unique<Server>();
        s->cfg = sc;
        if (!cfg.mcp.enabled || !sc.enabled || sc.command.empty()) {
            s->state = "disabled";
        } else {
            s->launch = build_launch(cfg, sc);
            ClientOptions o;
            o.name = sc.name;
            o.argv = s->launch.argv;
            o.env = s->launch.env;
            o.cwd = s->launch.cwd;
            o.protocol = sc.protocol;
            o.timeout_s = sc.timeout_s;
            o.startup_timeout_s = sc.startup_timeout_s;
            s->client = std::make_unique<Client>(o);
        }
        servers_.push_back(std::move(s));
    }
}

Manager::~Manager() {
    stopping_ = true;
    for (auto & s : servers_) if (s->client) s->client->on_tools_changed(nullptr);
    for (auto & s : servers_) if (s->client) s->client->shutdown();  // unblocks a handshake in progress
    for (auto & s : servers_) if (s->thr.joinable()) s->thr.join();
    for (auto & s : servers_) {
        std::vector<std::string> names;
        {
            std::lock_guard lk(s->mu);
            names = s->names;
        }
        for (auto & n : names) reg_.remove(n);
    }
}

void Manager::start() {
    for (const auto & id : cfg_.mcp.ignored_project_servers)
        std::fprintf(stderr, "mcp: ignoring [mcp.servers.%s] from ./.judas/config.toml (project files cannot start programs; "
                             "set JUDAS_PROJECT_MCP=1 to allow it)\n", id.c_str());
    for (auto & s : servers_) {
        if (!s->client) continue;
        Server * sp = s.get();
        sp->client->on_tools_changed([this, sp] { if (!stopping_) refresh(sp); });
        sp->thr = std::thread([this, sp] { connect(sp); });
    }
}

bool Manager::wait_ready(int timeout_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        bool pending = false;
        for (auto & s : servers_) {
            std::lock_guard lk(s->mu);
            if (s->state == "starting") pending = true;
        }
        if (!pending) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

void Manager::connect(Server * s) {
    std::string err;
    if (!s->client->start(err) || stopping_) {
        if (stopping_) return;
        std::lock_guard lk(s->mu);
        s->state = "failed";
        s->error = err;
        std::fprintf(stderr, "%s: failed to start: %s\n", log_name(s->cfg).c_str(), err.c_str());
        return;
    }
    refresh(s);
}

void Manager::refresh(Server * s) {
    std::lock_guard rl(s->refresh_mu);
    std::vector<ToolDef> defs;
    std::string err;
    if (!s->client->list_tools(defs, err)) {
        if (stopping_) return;
        std::lock_guard lk(s->mu);
        if (s->state != "ready") s->state = "failed";
        s->error = err;
        std::fprintf(stderr, "%s: tools/list failed: %s\n", log_name(s->cfg).c_str(), err.c_str());
        return;
    }
    const std::string prefix = s->cfg.prefix_set ? s->cfg.prefix : s->cfg.name + "__";
    std::vector<std::string> fresh;
    int filtered = 0;
    for (auto & d : defs) {
        bool allowed = s->cfg.tools_allow.empty();
        for (auto & g : s->cfg.tools_allow) if (glob_match(g, d.name)) allowed = true;
        for (auto & g : s->cfg.tools_deny) if (glob_match(g, d.name)) allowed = false;
        if (!allowed) { filtered++; continue; }
        std::string full = prefix + sanitize_name(d.name);
        if (const Tool * have = reg_.find(full); have && have->source != "mcp:" + s->cfg.name) {
            std::fprintf(stderr, "%s: tool '%s' would shadow an existing tool, skipped\n", log_name(s->cfg).c_str(), full.c_str());
            continue;
        }
        Tool t;
        t.name = full;
        t.source = "mcp:" + s->cfg.name;
        t.description = trim_description(d.description.empty() ? d.title : d.description, 300);
        if (t.description.empty()) t.description = "Tool " + d.name + " from MCP server " + s->cfg.name + ".";
        json schema = d.input_schema.is_object() ? d.input_schema : json::object();
        schema.erase("$schema");
        if (!schema.contains("type")) schema["type"] = "object";
        if (!schema.contains("properties")) schema["properties"] = json::object();
        t.parameters = schema;
        bool readonly = d.annotations.value("readOnlyHint", false);
        t.writes = false;
        t.may_be_irreversible = !readonly;
        std::string orig = d.name;
        t.run = [this, s, orig, readonly](const json & args, ToolContext & ctx) {
            return run_tool(s, orig, readonly, args, ctx);
        };
        reg_.add(std::move(t));
        fresh.push_back(full);
    }
    std::vector<std::string> old;
    {
        std::lock_guard lk(s->mu);
        old = s->names;
    }
    for (auto & n : old)
        if (std::find(fresh.begin(), fresh.end(), n) == fresh.end()) reg_.remove(n);
    std::lock_guard lk(s->mu);
    s->names = fresh;
    s->filtered = filtered;
    s->state = "ready";
    s->error.clear();
}

ToolResult Manager::run_tool(Server * s, const std::string & tool, bool readonly, const json & args, ToolContext & ctx) {
    auto fail = [](const std::string & m) { return ToolResult{false, m, false}; };
    // 1. SAFETY FLOOR, before anything reaches the server and in every permission mode.
    if (std::string m = floor_check(args, "", ctx, readonly); !m.empty()) return fail(m);
    // 2. Approval: a tool that may change things asks like the shell does (suggest, auto-edit), unless the server is trusted.
    if (!readonly && !s->cfg.trusted) {
        std::string mode = ctx.cfg ? ctx.cfg->permissions.mode : "auto-edit";
        if (mode != "full-auto") {
            std::string what = "MCP " + s->cfg.name + "/" + tool + " " + args.dump().substr(0, 200);
            if (!ctx.approve) return fail("not approved: " + what + " needs approval (permission mode " + mode + ") and no approver is available");
            if (!ctx.approve(what)) return fail("not approved: " + what + " was declined");
        }
    }
    if (!readonly && ctx.journal) ctx.journal->note_irreversible(ctx.checkpoint, "mcp " + s->cfg.name + "/" + tool);
    // 3. The call.
    CallOutcome o = s->client->call_tool(tool, args);
    if (!o.ok) return fail(o.error);
    std::string dir = expand_home(cfg_.data_dir) + "/mcp-files/" + (ctx.session.empty() ? "adhoc" : ctx.session);
    ToolResult r = convert_result(o.result, dir, s->cfg.name + "-" + tool, (size_t) std::max(200, cfg_.mcp.max_result_chars));
    r.irreversible = !readonly;
    return r;
}

std::vector<ServerStatus> Manager::status() const {
    std::vector<ServerStatus> out;
    for (auto & s : servers_) {
        ServerStatus st;
        st.name = s->cfg.name;
        {
            std::lock_guard lk(s->mu);
            st.state = s->state;
            st.error = s->error;
            st.tools_registered = (int) s->names.size();
            st.tools_filtered = s->filtered;
        }
        st.sandboxed = s->launch.sandboxed;
        st.sandbox_note = s->launch.note;
        if (s->client) {
            st.protocol = s->client->protocol_version();
            st.server_name = s->client->server_name();
            st.restarts = s->client->restarts();
        }
        out.push_back(std::move(st));
    }
    return out;
}

}  // namespace judas::mcp
