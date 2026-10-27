// See launch.h.
#include "mcp/launch.h"
#include "tools/tools.h"

#include <fnmatch.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>

namespace judas::mcp {

namespace fs = std::filesystem;

namespace {

std::string home() {
    const char * h = getenv("HOME");
    return h && *h ? h : "/root";
}

// Executable lookup: a name with a slash is used as is; otherwise PATH, then the usual user bin dirs
// (a systemd user unit often has a short PATH that lacks ~/.local/bin).
std::string resolve_exe(const std::string & cmd) {
    if (cmd.find('/') != std::string::npos) return cmd;
    std::vector<std::string> dirs;
    if (const char * p = getenv("PATH")) {
        std::string s = p;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t e = s.find(':', pos);
            if (e == std::string::npos) e = s.size();
            if (e > pos) dirs.push_back(s.substr(pos, e - pos));
            pos = e + 1;
        }
    }
    dirs.push_back(home() + "/.local/bin");
    dirs.push_back("/usr/local/bin");
    for (auto & d : dirs) {
        std::string c = d + "/" + cmd;
        if (access(c.c_str(), X_OK) == 0 && !fs::is_directory(c)) return c;
    }
    return cmd;
}

}  // namespace

std::string expand_value(const std::string & v) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '$' && i + 1 < v.size() && v[i + 1] == '{') {
            size_t e = v.find('}', i + 2);
            if (e != std::string::npos) {
                const char * val = getenv(v.substr(i + 2, e - i - 2).c_str());
                if (val) out += val;
                i = e;
                continue;
            }
        }
        out += v[i];
    }
    if (out == "~" || out.rfind("~/", 0) == 0) out = home() + out.substr(1);
    return out;
}

bool glob_match(const std::string & pattern, const std::string & text) {
    return fnmatch(pattern.c_str(), text.c_str(), 0) == 0;
}

SandboxVerdict sandbox_verdict(const McpServerConfig & s, const PermissionsConfig & p, bool bwrap_present) {
    SandboxVerdict v;
    if (!s.sandbox) {
        v.what = SandboxVerdict::What::unsandboxed;
        v.reason = "sandbox disabled for this server";
        return v;
    }
    if (p.sandbox == "bwrap" && bwrap_present) {
        v.what = SandboxVerdict::What::sandboxed;
        return v;
    }
    std::string why = p.sandbox != "bwrap" ? "permissions.sandbox is '" + p.sandbox + "'"
                                           : "bwrap (/usr/bin/bwrap) is not installed";
    if (p.allow_unsandboxed_mcp) {
        v.what = SandboxVerdict::What::unsandboxed;
        v.reason = why + "; allow_unsandboxed_mcp lets it run unconfined";
    } else {
        v.what = SandboxVerdict::What::refuse;
        v.reason = why + "; refusing to start it unconfined (set permissions.allow_unsandboxed_mcp = true to allow)";
    }
    return v;
}

namespace {

// Startup warning for the whole machine: with sandbox "none" neither shell commands nor MCP servers are
// confined. Once per process, one line (build_launch is per server, so this fires with the first server).
void warn_sandbox_none() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::fprintf(stderr, "judas: permissions.sandbox = \"none\": shell commands and MCP servers run unsandboxed\n");
    });
}

}  // namespace

Launch build_launch(const Config & cfg, const McpServerConfig & s) {
    Launch l;

    // Environment: nothing is inherited except these, so API keys in the daemon's environment do not leak
    // into a third-party server. Add what a server needs under env = { ... }.
    std::string path = getenv("PATH") ? getenv("PATH") : "/usr/local/bin:/usr/bin:/bin";
    path += ":" + home() + "/.local/bin";
    l.env.push_back("PATH=" + path);
    l.env.push_back("HOME=" + home());
    for (const char * k : {"USER", "LOGNAME", "LANG", "LC_ALL", "TERM"})
        if (const char * v = getenv(k)) l.env.push_back(std::string(k) + "=" + v);
    for (auto & [k, v] : s.env) {
        std::string e = k + "=" + expand_value(v);
        bool replaced = false;
        for (auto & have : l.env)
            if (have.rfind(k + "=", 0) == 0) { have = e; replaced = true; }
        if (!replaced) l.env.push_back(e);
    }

    std::vector<std::string> cmd = s.command;
    for (auto & c : cmd) c = expand_value(c);
    if (!cmd.empty()) cmd[0] = resolve_exe(cmd[0]);
    std::string cwd = expand_value(s.cwd);

    if (cfg.permissions.sandbox == "none") warn_sandbox_none();
    SandboxVerdict v = sandbox_verdict(s, cfg.permissions, !find_bwrap().empty());
    if (v.what == SandboxVerdict::What::refuse) {
        // Fail closed: an empty argv means the client can never spawn anything (see Client::start).
        std::fprintf(stderr, "mcp[%s]: refusing to start: %s\n", s.name.c_str(), v.reason.c_str());
        l.cwd = cwd;
        l.refused = true;
        l.note = "refused: " + v.reason;
        return l;
    }
    if (v.what == SandboxVerdict::What::unsandboxed) {
        l.argv = cmd;
        l.cwd = cwd;
        l.note = v.reason;
        return l;
    }

    std::vector<std::string> bargv = {find_bwrap(), "--die-with-parent", "--unshare-pid", "--unshare-uts",
                                      "--unshare-ipc", "--unshare-cgroup-try"};
    if (!s.network) bargv.push_back("--unshare-net");
    bargv.insert(bargv.end(), {"--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp"});
    std::error_code ec;
    for (auto & w : s.writable) {
        std::string p = expand_value(w);
        if (!p.empty() && p != "/" && fs::exists(p, ec)) bargv.insert(bargv.end(), {"--bind", p, p});
    }
    // Hide secrets (reads and writes both), like the shell sandbox, plus Judas's own config and data.
    std::string h = home();
    std::vector<std::string> hide = {h + "/.ssh", h + "/.gnupg", h + "/.config/judas", expand_value(cfg.data_dir)};
    for (const auto & sp : cfg.permissions.secret_paths)
        if (!sp.empty() && sp.find('*') == std::string::npos) hide.push_back(expand_value(sp));
    for (const std::string & d : hide)
        if (fs::is_directory(d, ec)) bargv.insert(bargv.end(), {"--tmpfs", d});
    // Many tools want a cache dir; give them a throwaway one instead of the real one.
    if (fs::is_directory(h + "/.cache", ec)) bargv.insert(bargv.end(), {"--tmpfs", h + "/.cache"});
    if (!cwd.empty()) bargv.insert(bargv.end(), {"--chdir", cwd});
    bargv.push_back("--");
    bargv.insert(bargv.end(), cmd.begin(), cmd.end());
    l.argv = bargv;
    l.sandboxed = true;
    l.note = std::string("bwrap: read-only root, ") + (s.network ? "network on" : "network off") +
             (s.writable.empty() ? ", nothing writable" : ", " + std::to_string(s.writable.size()) + " writable path(s)");
    return l;
}

}  // namespace judas::mcp
