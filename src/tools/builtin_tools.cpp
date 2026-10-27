// Built-in tools: files, search, shell, web, snapshots.
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "cpp-httplib/httplib.h"
#include "tools/tools.h"

namespace judas {
namespace fs = std::filesystem;

std::string find_bwrap();

namespace {

// ---------- small helpers ----------

std::string home_dir() {
    const char * h = getenv("HOME");
    return h ? h : "/root";
}

std::string expand(const std::string & p) {
    if (p == "~") return home_dir();
    if (p.rfind("~/", 0) == 0) return home_dir() + p.substr(1);
    return p;
}

const Config & cfg_of(const ToolContext & ctx) {
    static const Config def;
    return ctx.cfg ? *ctx.cfg : def;
}

fs::path resolve_path(const ToolContext & ctx, const std::string & path) {
    fs::path p = expand(path);
    if (p.is_relative()) p = fs::path(ctx.cwd.empty() ? "." : ctx.cwd) / p;
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

bool under(const fs::path & p, const fs::path & dir) {
    auto pi = p.begin(), di = dir.begin();
    for (; di != dir.end(); ++di, ++pi) {
        if (di->empty()) continue;  // trailing slash
        if (pi == p.end() || *pi != *di) return false;
    }
    return true;
}

// Secrets nobody (model included) may read or write, even with an empty config.
std::string hard_deny(const fs::path & p, const PermissionsConfig & perm) {
    std::string s = p.string();
    for (const auto & sp : perm.secret_paths) {
        if (sp.empty()) continue;
        std::string e = expand(sp);
        if (e.find('*') != std::string::npos) {
            if (fnmatch(e.c_str(), s.c_str(), 0) == 0) return "path matches secret_paths entry " + sp;
        } else if (under(p, fs::path(e))) {
            return "path is under " + sp + " (secret_paths)";
        }
    }
    // A directory spelled .ssh or .gnupg anywhere in the path hides keys, whatever its parent is: a symlink
    // or a "a/../.ssh/..." spelling must not carry a path past the deny list only by resolving differently.
    for (const fs::path & part : p) {
        std::string c = part.string();
        if (c == ".ssh" || c == ".gnupg") return "path goes through " + c;
    }
    std::string base = p.filename().string();
    for (const char * pat : {".env", "*.env", "id_rsa*", "id_ed25519*", "*.pem"}) {
        if (fnmatch(pat, base.c_str(), 0) == 0) return "secret file name (" + base + ")";
    }
    return "";
}

// Judas's own state: no tool may write its config, secrets or data (a forged checkpoint or an edited
// permissions table would be an escalation), and no tool may read its secrets.
std::string judas_internal(const ToolContext & ctx, const fs::path & p, bool write) {
    fs::path conf = fs::path(home_dir()) / ".config" / "judas";
    if (under(p, conf / "secrets.env")) return "Judas's secrets file is off limits";
    if (!write) return "";
    if (under(p, conf / "config.toml") || under(p, fs::path(expand(cfg_of(ctx).data_dir))))
        return "Judas's own config and data cannot be changed by a tool";
    std::string base = p.filename().string(), parent = p.parent_path().filename().string();
    if (base == "config.toml" && parent == ".judas") return "a project's .judas/config.toml cannot be changed by a tool";
    return "";
}

std::string read_denied(const ToolContext & ctx, const std::string & path, fs::path & resolved) {
    resolved = resolve_path(ctx, path);
    if (std::string r = judas_internal(ctx, resolved, false); !r.empty()) return "refused: " + r;
    // Both spellings count: the given path, so "a/../.ssh/id_rsa" or "~/link" cannot rename itself into
    // something the deny list cannot see even if resolution ends elsewhere, and the resolved path, so a
    // symlink pointing at a secret directory is refused.
    const auto & perm = cfg_of(ctx).permissions;
    if (std::string r = hard_deny(fs::path(expand(path)), perm); !r.empty()) return "refused: " + r;
    if (std::string r = hard_deny(resolved, perm); !r.empty()) return "refused: " + r;
    return "";
}

ToolResult fail(const std::string & m) { return {false, m, false}; }
ToolResult okr(const std::string & m) { return {true, m, false}; }

std::string cap_text(const std::string & s, size_t max) {
    if (s.size() <= max) return s;
    size_t h = max / 2, t = max - h;
    return s.substr(0, h) + "\n[... " + std::to_string(s.size() - max) + " chars truncated ...]\n" + s.substr(s.size() - t);
}

// Approval policy: "suggest" asks for writes and shell, "auto-edit" asks for shell only, "full-auto" never.
// Returns "" if allowed, else the failure message.
std::string need_approval(ToolContext & ctx, bool is_write, bool is_shell, const std::string & what) {
    const std::string & mode = cfg_of(ctx).permissions.mode;
    bool ask = (mode == "full-auto") ? false : (mode == "suggest" ? (is_write || is_shell) : is_shell);
    if (!ask) return "";
    if (!ctx.approve) return "not approved: " + what + " needs approval (permission mode " + mode + ") and no approver is available";
    if (!ctx.approve(what)) return "not approved: " + what + " was declined";
    return "";
}

std::string read_whole(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool looks_binary(const std::string & s) {
    size_t n = std::min<size_t>(s.size(), 8000);
    return memchr(s.data(), 0, n) != nullptr;
}

bool skip_dir(const std::string & n) {
    return n == ".git" || n == "node_modules" || n == "build" || n.rfind("build-", 0) == 0 || n == "__pycache__" ||
           n == ".venv" || n == "target" || n == ".cache";
}

// ---------- process runner ----------

struct ProcResult {
    int code = -1;
    bool timed_out = false;
    bool start_failed = false;
    std::string output;
};

// fork/exec with a pipe; the child leads its own process group so a timeout kills the whole tree.
ProcResult run_process(const std::vector<std::string> & argv, const std::string & cwd, int timeout_s, bool merge_stderr) {
    ProcResult r;
    int fds[2];
    if (pipe(fds) != 0) { r.start_failed = true; r.output = "pipe failed"; return r; }
    std::vector<char *> av;
    for (auto & a : argv) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); r.start_failed = true; r.output = "fork failed"; return r; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        if (merge_stderr) dup2(fds[1], 2);
        else { int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 2); }
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0) dup2(in, 0);
        close(fds[0]); close(fds[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execvp(av[0], av.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    int fd = fds[0];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    const size_t hard_limit = 8u << 20;
    char buf[8192];
    int status = 0;
    bool exited = false;
    while (true) {
        struct pollfd pf{fd, POLLIN, 0};
        int pr = poll(&pf, 1, 100);
        if (pr > 0) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) { if (r.output.size() < hard_limit) r.output.append(buf, n); continue; }
            if (n == 0) break;  // EOF
        }
        if (!exited && waitpid(pid, &status, WNOHANG) == pid) exited = true;
        if (exited && pr <= 0) break;  // child gone and nothing left to drain
        if (std::chrono::steady_clock::now() > deadline) {
            r.timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
    }
    close(fd);
    if (!exited) { waitpid(pid, &status, 0); }
    if (r.timed_out) r.code = -1;
    else if (WIFEXITED(status)) r.code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.code = 128 + WTERMSIG(status);
    return r;
}

// ---------- glob ----------

std::regex glob_to_regex(const std::string & g) {
    std::string re;
    for (size_t i = 0; i < g.size(); i++) {
        char c = g[i];
        if (c == '*') {
            if (i + 1 < g.size() && g[i + 1] == '*') {
                i++;
                if (i + 1 < g.size() && g[i + 1] == '/') { i++; re += "(?:.*/)?"; }
                else re += ".*";
            } else re += "[^/]*";
        } else if (c == '?') re += "[^/]";
        else if (c == '{') re += "(?:";
        else if (c == '}') re += ")";
        else if (c == ',') {
            // comma inside braces is alternation
            int depth = 0;
            for (size_t j = 0; j < i; j++) { if (g[j] == '{') depth++; if (g[j] == '}') depth--; }
            re += depth > 0 ? "|" : ",";
        } else if (strchr(".^$+()[]|\\", c)) { re += '\\'; re += c; }
        else re += c;
    }
    return std::regex(re);
}

std::vector<std::pair<fs::file_time_type, fs::path>> walk_files(const fs::path & root, const std::function<bool(const fs::path &)> & want) {
    std::vector<std::pair<fs::file_time_type, fs::path>> out;
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        if (want(root.filename())) out.push_back({fs::last_write_time(root, ec), root});
        return out;
    }
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (it->is_directory(e2)) {
            if (skip_dir(it->path().filename().string())) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        fs::path rel = fs::relative(it->path(), root, e2);
        if (want(rel)) out.push_back({fs::last_write_time(it->path(), e2), it->path()});
    }
    return out;
}

// ---------- tool implementations ----------

ToolResult t_read_file(const json & a, ToolContext & ctx) {
    fs::path p;
    std::string d = read_denied(ctx, a.at("path").get<std::string>(), p);
    if (!d.empty()) return fail(d);
    std::error_code ec;
    if (fs::is_directory(p, ec)) return fail(p.string() + " is a directory; use list_dir");
    if (!fs::exists(p, ec)) return fail("no such file: " + p.string());
    std::string data = read_whole(p);
    if (looks_binary(data)) return fail(p.string() + " looks like a binary file (" + std::to_string(data.size()) + " bytes); not shown");
    long offset = std::max(1L, a.value("offset", 1L));
    long limit = std::max(1L, a.value("limit", 2000L));
    std::vector<std::string> lines;
    {
        size_t s = 0;
        while (s < data.size()) {
            size_t e = data.find('\n', s);
            if (e == std::string::npos) e = data.size();
            lines.push_back(data.substr(s, e - s));
            s = e + 1;
        }
    }
    long total = (long) lines.size();
    // Character budget per call, so one read cannot blow an agent's context: about a quarter of ctx_per_agent
    // (~3 chars per token), at most 30k chars. Stops on a whole line and says where to continue.
    size_t budget = 30000;
    try {
        int cap = cfg_of(ctx).active_model().ctx_per_agent;
        if (cap > 0) budget = std::clamp<size_t>((size_t) cap * 3 / 4, 4000, 30000);
    } catch (...) {}
    std::ostringstream o;
    long last = std::min(total, offset - 1 + limit);
    size_t used = 0;
    for (long i = offset; i <= last; i++) {
        std::string l = lines[i - 1];
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.size() > 2000) l = l.substr(0, 2000) + " [line truncated]";
        char num[32];
        snprintf(num, sizeof num, "%6ld\t", i);
        if (i > offset && used + l.size() + 8 > budget) { last = i - 1; break; }
        used += l.size() + 8;
        o << num << l << "\n";
    }
    if (offset > total && total > 0) o << "(offset " << offset << " is past the end)\n";
    o << "[" << p.string() << ": " << total << " lines total";
    if (last < total) o << ", showing " << offset << "-" << last << ", use offset/limit for more";
    o << "]";
    return okr(o.str());
}

// Shared prelude of writing tools: policy, approval, journal.
std::string prepare_write(ToolContext & ctx, const std::string & tool, const std::string & path, fs::path & out) {
    std::string resolved;
    std::string why = check_write_path(ctx, path, resolved);
    if (!why.empty()) return "refused: " + why;
    out = resolved;
    std::string ap = need_approval(ctx, true, false, tool + " " + resolved);
    if (!ap.empty()) return ap;
    if (ctx.journal) ctx.journal->record(ctx.checkpoint, out);
    return "";
}

void write_whole(const fs::path & p, const std::string & content) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot open for writing: " + p.string());
    f << content;
    if (!f) throw std::runtime_error("write failed: " + p.string());
}

ToolResult t_write_file(const json & a, ToolContext & ctx) {
    fs::path p;
    std::string e = prepare_write(ctx, "write_file", a.at("path").get<std::string>(), p);
    if (!e.empty()) return fail(e);
    std::string content = a.at("content").get<std::string>();
    std::error_code ec;
    if (fs::is_directory(p, ec)) return fail(p.string() + " is a directory");
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    bool existed = fs::exists(p, ec);
    if (a.value("append", false)) {
        std::ofstream f(p, std::ios::binary | std::ios::app);
        if (!f) return fail("cannot open for appending: " + p.string());
        f << content;
        if (!f) return fail("append failed: " + p.string());
        return okr(std::string(existed ? "appended " : "created ") + std::to_string(content.size()) + " bytes to " + p.string());
    }
    write_whole(p, content);
    return okr(std::string(existed ? "overwrote " : "created ") + p.string() + " (" + std::to_string(content.size()) + " bytes)");
}

ToolResult t_edit_file(const json & a, ToolContext & ctx) {
    fs::path p;
    std::string path = a.at("path").get<std::string>();
    std::string rs;
    std::string why = check_write_path(ctx, path, rs);
    if (!why.empty()) return fail("refused: " + why);
    p = rs;
    std::string oldS = a.at("old_string").get<std::string>(), newS = a.at("new_string").get<std::string>();
    bool all = a.value("replace_all", false);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return fail("no such file: " + p.string());
    if (oldS.empty()) return fail("old_string is empty");
    if (oldS == newS) return fail("old_string and new_string are identical");
    std::string data = read_whole(p);
    size_t count = 0;
    for (size_t pos = data.find(oldS); pos != std::string::npos; pos = data.find(oldS, pos + oldS.size())) count++;
    if (count == 0) return fail("old_string not found in " + p.string() + " (it must match exactly, including whitespace)");
    if (count > 1 && !all) return fail("old_string occurs " + std::to_string(count) + " times in " + p.string() + "; add surrounding context to make it unique, or set replace_all");
    std::string ap = need_approval(ctx, true, false, "edit_file " + p.string());
    if (!ap.empty()) return fail(ap);
    if (ctx.journal) ctx.journal->record(ctx.checkpoint, p);
    std::string out;
    size_t s = 0;
    for (size_t pos = data.find(oldS); pos != std::string::npos; pos = data.find(oldS, s)) {
        out.append(data, s, pos - s);
        out += newS;
        s = pos + oldS.size();
        if (!all) break;
    }
    out.append(data, s, std::string::npos);
    write_whole(p, out);
    return okr("edited " + p.string() + " (" + std::to_string(all ? count : 1) + " replacement" + ((all ? count : 1) == 1 ? "" : "s") + ")");
}

ToolResult t_list_dir(const json & a, ToolContext & ctx) {
    fs::path p;
    std::string d = read_denied(ctx, a.value("path", std::string(".")), p);
    if (!d.empty()) return fail(d);
    int depth = std::max(1, a.value("depth", 2));
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return fail("not a directory: " + p.string());
    const size_t cap = 500;
    size_t n = 0;
    bool truncated = false;
    std::ostringstream o;
    o << p.string() << "/\n";
    std::function<void(const fs::path &, int, const std::string &)> rec = [&](const fs::path & dir, int lvl, const std::string & ind) {
        std::vector<fs::directory_entry> es;
        std::error_code e;
        for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, e), end; !e && it != end; it.increment(e)) es.push_back(*it);
        std::sort(es.begin(), es.end(), [](auto & x, auto & y) {
            std::error_code e1;
            bool dx = x.is_directory(e1), dy = y.is_directory(e1);
            if (dx != dy) return dx;
            return x.path().filename() < y.path().filename();
        });
        for (auto & en : es) {
            if (truncated) return;
            std::string name = en.path().filename().string();
            std::error_code e2;
            bool isdir = en.is_directory(e2);
            if (isdir && skip_dir(name)) continue;
            if (++n > cap) { truncated = true; return; }
            o << ind << name << (isdir ? "/" : "") << "\n";
            if (isdir && lvl < depth && !en.is_symlink(e2)) rec(en.path(), lvl + 1, ind + "  ");
        }
    };
    rec(p, 1, "  ");
    if (truncated) o << "[truncated at " << cap << " entries; list a subdirectory]\n";
    return okr(o.str());
}

ToolResult t_glob(const json & a, ToolContext & ctx) {
    fs::path root;
    std::string d = read_denied(ctx, a.value("path", std::string(".")), root);
    if (!d.empty()) return fail(d);
    std::string pat = a.at("pattern").get<std::string>();
    std::error_code ec;
    if (!fs::exists(root, ec)) return fail("no such path: " + root.string());
    bool has_slash = pat.find('/') != std::string::npos;
    std::regex re = glob_to_regex(pat);
    auto files = walk_files(root, [&](const fs::path & rel) {
        std::string s = has_slash ? rel.generic_string() : rel.filename().string();
        return std::regex_match(s, re);
    });
    std::sort(files.begin(), files.end(), [](auto & x, auto & y) { return x.first > y.first; });
    std::ostringstream o;
    size_t shown = 0;
    for (auto & f : files) {
        if (!hard_deny(f.second, cfg_of(ctx).permissions).empty()) continue;
        if (shown++ >= 200) break;
        o << f.second.string() << "\n";
    }
    if (files.empty()) return okr("no files match " + pat + " under " + root.string());
    if (files.size() > 200) o << "[showing 200 of " << files.size() << " matches, newest first]\n";
    return okr(o.str());
}

ToolResult t_grep(const json & a, ToolContext & ctx) {
    fs::path root;
    std::string d = read_denied(ctx, a.value("path", std::string(".")), root);
    if (!d.empty()) return fail(d);
    std::string pat = a.at("pattern").get<std::string>();
    std::string gl = a.value("glob", std::string());
    bool icase = a.value("ignore_case", false);
    size_t maxr = (size_t) std::max(1, a.value("max_results", 200));
    std::vector<std::string> lines;
    bool have_rg = run_process({"rg", "--version"}, "", 5, false).code == 0;
    if (have_rg) {
        std::vector<std::string> argv = {"rg", "--line-number", "--no-heading", "--color", "never", "--max-columns", "300", "--glob", "!.git"};
        if (icase) argv.push_back("-i");
        if (!gl.empty()) { argv.push_back("--glob"); argv.push_back(gl); }
        argv.insert(argv.end(), {"-e", pat, "--", root.string()});
        ProcResult r = run_process(argv, ctx.cwd, 60, false);
        if (r.code == 2 && r.output.empty()) return fail("grep: invalid pattern or path");
        std::istringstream in(r.output);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    } else {
        std::regex re;
        try { re = std::regex(pat, icase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript); }
        catch (const std::regex_error & e) { return fail(std::string("invalid regex: ") + e.what()); }
        std::regex gre;
        if (!gl.empty()) gre = glob_to_regex(gl);
        bool gslash = gl.find('/') != std::string::npos;
        auto files = walk_files(root, [&](const fs::path & rel) {
            if (gl.empty()) return true;
            return std::regex_match(gslash ? rel.generic_string() : rel.filename().string(), gre);
        });
        std::sort(files.begin(), files.end(), [](auto & x, auto & y) { return x.second < y.second; });
        for (auto & f : files) {
            if (lines.size() > maxr) break;
            std::string data = read_whole(f.second);
            if (looks_binary(data)) continue;
            std::istringstream in(data);
            std::string l;
            for (int n = 1; std::getline(in, l); n++) {
                if (std::regex_search(l, re)) lines.push_back(f.second.string() + ":" + std::to_string(n) + ":" + l.substr(0, 300));
                if (lines.size() > maxr) break;
            }
        }
    }
    std::ostringstream o;
    size_t shown = 0;
    for (auto & l : lines) {
        // drop hits in hard-denied files
        size_t c = l.find(':');
        if (c != std::string::npos && !hard_deny(fs::path(l.substr(0, c)), cfg_of(ctx).permissions).empty()) continue;
        if (shown++ >= maxr) break;
        o << l << "\n";
    }
    if (lines.empty()) return okr("no matches for " + pat);
    if (lines.size() > maxr) o << "[truncated at " << maxr << " results]\n";
    return okr(o.str());
}

// Always blocked, in EVERY permission mode including full-auto. Shell access is fine; privilege escalation and
// credential handling are not. Added 2026-10-13 after a worker, told only to write files, invented a sudo
// password brute-force loop that tripped pam_faillock and locked the real user out of sudo.
std::string blocked_command(const std::string & cmd) {
    // Program names that must never be invoked by an agent, matched in command position only (start, after ; & | ( ` { $( or a launcher like env/exec/xargs), optionally quoted,
    // escaped or spelled with a bin path; so "getent passwd" or "man sudo" pass.
    static const std::regex re(
        R"((^|[;&|(`{\n]|\$\()\s*((env|exec|command|nohup|time|nice|xargs|setsid|stdbuf|timeout|do|then|else|!)(\s+-\S+|\s+\w+=\S*)*(\s+\d+\S*)?\s+)*)"
        R"((\w+=\S*\s+)*["'\\]?(/\S*/)?(sudo|doas|pkexec|su|visudo|passwd|chpasswd|usermod|faillock|pam_tally2|run0)["']?)"
        R"(([\s;&|)]|$))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(cmd, m, re)) return "refused: privilege escalation and credential commands are blocked ('" + m[8].str() + "'). Shell is allowed, sudo is not. If a step truly needs root, stop and report it to the user.";
    // The auth databases: the secret ones may not be named at all; the readable ones may not be written by any
    // common verb. A string check is a courtesy, not the floor: inside bwrap no_new_privs stops setuid tools.
    for (const char * p : {"/etc/shadow", "/etc/gshadow", "/etc/sudoers", "/var/run/faillock", "/run/faillock",
                           "/etc/security/opasswd"})
        if (cmd.find(p) != std::string::npos) return std::string("refused: the auth database (") + p + ") is off limits.";
    static const std::regex writes(R"((>>?\s*/etc/(passwd|group)\b|\btee\b|\bcp\b|\bmv\b|\binstall\b|\bdd\b|\bsed\s+-i|\btruncate\b|\bln\b|\bch(mod|own)\b|\brm\b))");
    for (const char * p : {"/etc/passwd", "/etc/group"})
        if (cmd.find(p) != std::string::npos && std::regex_search(cmd, writes))
            return std::string("refused: writing to the auth database (") + p + ") is blocked.";
    return "";
}

// Build the argv for a shell command. In sandbox "bwrap" mode the command runs in a bubblewrap namespace:
// the whole filesystem read-only, each allow_path (and the cwd) bound read-write, a set of secret dirs hidden
// behind tmpfs, and a fresh /tmp. So a shell command cannot write outside the allowed tree, even in full-auto.
// Sandbox "bwrap" with no bwrap installed returns an empty argv: the caller refuses instead of running unconfined.
std::vector<std::string> shell_argv(const ToolContext & ctx, const std::string & cmd, const std::string & cwd) {
    const auto & perm = cfg_of(ctx).permissions;
    if (perm.sandbox != "bwrap") {
        // No sandbox: still set no_new_privs when setpriv exists, so sudo and other setuid tools cannot elevate.
        if (::access("/usr/bin/setpriv", X_OK) == 0) return {"/usr/bin/setpriv", "--no-new-privs", "/bin/bash", "-lc", cmd};
        return {"/bin/bash", "-lc", cmd};
    }
    std::string bw = find_bwrap();
    if (bw.empty()) return {};
    std::vector<std::string> v = {bw, "--die-with-parent", "--unshare-pid", "--unshare-uts",
                                  "--unshare-ipc", "--unshare-cgroup-try"};
    if (!perm.sandbox_net) v.push_back("--unshare-net");
    // root read-only, then standard virtual filesystems
    v.insert(v.end(), {"--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp"});
    // writable: the configured allow paths and the working directory
    auto add_rw = [&](const std::string & path) {
        std::string p = expand(path);
        std::error_code ec;
        if (!p.empty() && p != "/" && fs::exists(p, ec)) v.insert(v.end(), {"--bind", p, p});
    };
    for (const auto & p : perm.allow_paths) add_rw(p);
    // the working directory is writable only when the write policy allows it (a model-chosen cwd like /etc
    // must not become a read-write bind)
    std::string cwd_resolved;
    if (!cwd.empty() && check_write_path(ctx, cwd, cwd_resolved).empty()) add_rw(cwd);
    // hide secrets from the sandboxed shell (reads and writes both), on top of the rw binds
    // and Judas's own config (API tokens in secrets.env) and data
    std::vector<std::string> hide = {home_dir() + "/.ssh", home_dir() + "/.gnupg", home_dir() + "/.config/judas",
                                     expand(cfg_of(ctx).data_dir)};
    for (const auto & sp : perm.secret_paths)
        if (!sp.empty() && sp.find('*') == std::string::npos) hide.push_back(expand(sp));
    for (const std::string & d : hide) {
        std::error_code ec;
        if (fs::is_directory(d, ec)) v.insert(v.end(), {"--tmpfs", d});
    }
    if (!cwd.empty()) v.insert(v.end(), {"--chdir", cwd});
    v.insert(v.end(), {"/bin/bash", "-lc", cmd});
    return v;
}

ToolResult t_shell(const json & a, ToolContext & ctx) {
    std::string cmd = a.at("command").get<std::string>();
    const auto & perm = cfg_of(ctx).permissions;
    if (std::string b = blocked_command(cmd); !b.empty()) return fail(b);
    for (auto & bad : perm.deny_commands) {
        if (!bad.empty() && cmd.find(bad) != std::string::npos) return fail("refused: command contains denied text '" + bad + "'");
    }
    std::string ap = need_approval(ctx, false, true, "shell: " + cmd);
    if (!ap.empty()) return fail(ap);
    int timeout = a.value("timeout_s", perm.shell_timeout_s);
    if (timeout < 1) timeout = perm.shell_timeout_s;
    std::string cwd = ctx.cwd;
    if (a.contains("cwd") && a["cwd"].is_string()) cwd = resolve_path(ctx, a["cwd"].get<std::string>()).string();
    if (ctx.journal) ctx.journal->note_irreversible(ctx.checkpoint, "shell: " + cmd);
    auto argv = shell_argv(ctx, cmd, cwd);
    if (argv.empty())
        return fail("refused: permissions.sandbox is \"bwrap\" but bwrap is not installed. Install bubblewrap, or set "
                    "permissions.sandbox = \"none\" to run shell commands unconfined.");
    ProcResult r = run_process(argv, cwd, timeout, true);
    ToolResult res;
    res.irreversible = true;
    std::string out = cap_text(r.output, 30000);
    if (r.timed_out) {
        res.ok = false;
        res.output = out + "\n[timed out after " + std::to_string(timeout) + "s, process group killed]";
    } else {
        res.ok = r.code == 0;
        res.output = out + (out.empty() || out.back() == '\n' ? "" : "\n") + "[exit code " + std::to_string(r.code) + "]";
    }
    return res;
}

std::string url_encode(const std::string & s) {
    static const char * hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char) c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

ToolResult t_web_search(const json & a, ToolContext & ctx) {
    std::string q = a.at("query").get<std::string>();
    int maxr = std::max(1, a.value("max_results", 8));
    std::string base = cfg_of(ctx).search.searxng;
    httplib::Client cli(base);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(20);
    auto res = cli.Get("/search?q=" + url_encode(q) + "&format=json");
    if (!res) return fail("web_search: SearXNG at " + base + " is not reachable (is the searxng service running?)");
    if (res->status != 200) return fail("web_search: SearXNG returned HTTP " + std::to_string(res->status) + " (format=json may be disabled)");
    json j = json::parse(res->body, nullptr, false);
    if (j.is_discarded() || !j.contains("results")) return fail("web_search: unexpected reply from SearXNG");
    std::ostringstream o;
    int n = 0;
    for (auto & r : j["results"]) {
        if (n++ >= maxr) break;
        o << n << ". " << r.value("title", std::string()) << "\n   " << r.value("url", std::string()) << "\n";
        std::string c = r.value("content", std::string());
        if (!c.empty()) o << "   " << c.substr(0, 400) << "\n";
    }
    if (n == 0) return okr("no results for: " + q);
    return okr(o.str());
}

ToolResult t_web_fetch(const json & a, ToolContext & ctx) {
    std::string url = a.at("url").get<std::string>();
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return fail("web_fetch: url must start with http:// or https://");
    size_t maxc = (size_t) std::max(100, a.value("max_chars", 20000));
    std::string cmd = expand(cfg_of(ctx).search.fetch_command);
    if (cmd.empty()) cmd = "webfetch";
    std::vector<std::string> argv = {cmd};
    if (fs::path(cmd).filename() == "webfetch") { argv.push_back("--max"); argv.push_back(std::to_string(maxc)); }
    argv.push_back(url);
    ProcResult r = run_process(argv, ctx.cwd, 90, true);
    if (r.timed_out) return fail("web_fetch: timed out fetching " + url);
    if (r.code == 127) return fail("web_fetch: fetch command '" + cmd + "' not found");
    if (r.code != 0) return fail("web_fetch: " + cmd + " exited " + std::to_string(r.code) + ": " + cap_text(r.output, 1000));
    return okr(cap_text(r.output, maxc));
}

ToolResult need_journal(Journal * j, bool & bad) {
    bad = j == nullptr;
    return fail("snapshots are not available (no journal)");
}

ToolResult t_snapshot_list(const json & a, ToolContext & ctx) {
    bool bad;
    ToolResult nj = need_journal(ctx.journal, bad);
    if (bad) return nj;
    size_t limit = (size_t) std::max(1, a.value("limit", 20));
    auto cps = ctx.journal->list(ctx.session, limit);
    if (cps.empty()) return okr("no checkpoints in this session");
    std::ostringstream o;
    for (auto & c : cps) {
        o << c.id << "  " << c.label << "  files=" << c.files.size();
        if (!c.irreversible.empty()) o << "  irreversible=" << c.irreversible.size();
        if (c.reverted) o << "  [reverted]";
        o << "\n";
    }
    return okr(o.str());
}

ToolResult t_snapshot_diff(const json & a, ToolContext & ctx) {
    bool bad;
    ToolResult nj = need_journal(ctx.journal, bad);
    if (bad) return nj;
    std::string id = a.at("id").get<std::string>();
    if (!ctx.journal->get(id)) return fail("no such checkpoint: " + id);
    std::string d = ctx.journal->diff(id);
    return okr(d.empty() ? "no differences" : cap_text(d, 30000));
}

ToolResult t_snapshot_revert(const json & a, ToolContext & ctx) {
    bool bad;
    ToolResult nj = need_journal(ctx.journal, bad);
    if (bad) return nj;
    std::string id = a.at("id").get<std::string>();
    auto cp = ctx.journal->get(id);
    if (!cp) return fail("no such checkpoint: " + id);
    if (!ctx.session.empty() && cp->session != ctx.session)
        return fail("checkpoint " + id + " belongs to another session; revert it from that session or the Snapshots screen");
    std::string ap = need_approval(ctx, true, false, "snapshot_revert " + id);
    if (!ap.empty()) return fail(ap);
    bool to = a.value("to", false);
    RevertResult r = to ? ctx.journal->revert_to(id, ctx.session) : ctx.journal->revert(id);
    std::ostringstream o;
    auto list = [&](const char * title, const std::vector<std::string> & v) {
        if (v.empty()) return;
        o << title << " (" << v.size() << "):\n";
        for (auto & s : v) o << "  " << s << "\n";
    };
    list("restored", r.restored);
    list("removed", r.removed);
    list("conflicts (changed since the checkpoint, restored anyway)", r.conflicts);
    list("irreversible (not undone)", r.irreversible);
    if (!r.error.empty()) o << "error: " << r.error << "\n";
    if (r.restored.empty() && r.removed.empty() && r.error.empty()) o << "nothing to restore\n";
    ToolResult res;
    res.ok = r.ok;
    res.output = o.str();
    return res;
}

json obj(json props, std::vector<std::string> required) {
    json j = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array()}};
    for (auto & r : required) j["required"].push_back(r);
    return j;
}
json str(const char * d) { return {{"type", "string"}, {"description", d}}; }
json num(const char * d) { return {{"type", "integer"}, {"description", d}}; }
json bln(const char * d) { return {{"type", "boolean"}, {"description", d}}; }

}  // namespace

std::string check_write_path(const ToolContext & ctx, const std::string & path, std::string & resolved) {
    fs::path p = resolve_path(ctx, path);
    resolved = p.string();
    std::string h = hard_deny(p, cfg_of(ctx).permissions);
    if (!h.empty()) return h;
    if (std::string j = judas_internal(ctx, p, true); !j.empty()) return j;
    const auto & perm = cfg_of(ctx).permissions;
    // deny globs: the path or any parent directory matching refuses
    for (auto & dp : perm.deny_paths) {
        std::string pat = expand(dp);
        fs::path cur;
        for (auto & part : p) {
            cur /= part;
            if (fnmatch(pat.c_str(), cur.string().c_str(), 0) == 0) return "path matches deny_paths entry '" + dp + "'";
        }
    }
    bool allowed = false;
    for (auto & ap : perm.allow_paths) {
        std::error_code ec;
        fs::path a = fs::weakly_canonical(expand(ap), ec);
        if (ec) a = expand(ap);
        if (under(p, a)) { allowed = true; break; }
    }
    if (!allowed) return "path " + resolved + " is outside the allowed paths (permissions.allow_paths)";
    return "";
}

void register_builtin_tools(ToolRegistry & reg) {
    auto add = [&](const char * name, const char * desc, json params, bool writes, bool irrev,
                   ToolResult (*fn)(const json &, ToolContext &)) {
        Tool t;
        t.name = name;
        t.description = desc;
        t.parameters = std::move(params);
        t.writes = writes;
        t.may_be_irreversible = irrev;
        t.run = fn;
        reg.add(std::move(t));
    };
    add("read_file", "Read a text file with line numbers. Default 2000 lines; use offset (1-based line) and limit for more.",
        obj({{"path", str("File path, relative to the working directory or absolute")},
             {"offset", num("First line to show, 1-based")}, {"limit", num("Max lines to show")}}, {"path"}),
        false, false, t_read_file);
    add("write_file", "Create or overwrite a file with the given content. Creates parent directories. Journaled, so it can be reverted. For a large file write the first part, then call again with append=true for each next part (a few thousand characters per call, so no call is cut off).",
        obj({{"path", str("File path")}, {"content", str("File content, or the next part when append is true")},
             {"append", bln("Add content to the end of the file instead of replacing it")}}, {"path", "content"}), true, false, t_write_file);
    add("edit_file", "Replace an exact string in a file. Fails if old_string is missing or not unique unless replace_all is true.",
        obj({{"path", str("File path")}, {"old_string", str("Exact text to find")}, {"new_string", str("Replacement text")},
             {"replace_all", bln("Replace every occurrence")}}, {"path", "old_string", "new_string"}), true, false, t_edit_file);
    add("list_dir", "List a directory as a tree (skips .git, node_modules, build dirs). Default depth 2, at most 500 entries.",
        obj({{"path", str("Directory, default the working directory")}, {"depth", num("Levels to show, default 2")}}, {}),
        false, false, t_list_dir);
    add("glob", "Find files by glob pattern (supports ** and {a,b}), newest first, at most 200.",
        obj({{"pattern", str("Glob such as src/**/*.cpp or *.md")}, {"path", str("Directory to search, default the working directory")}}, {"pattern"}),
        false, false, t_glob);
    add("grep", "Search file contents with a regex. Returns file:line:text lines.",
        obj({{"pattern", str("Regular expression")}, {"path", str("File or directory, default the working directory")},
             {"glob", str("Only search files matching this glob, e.g. *.cpp")}, {"ignore_case", bln("Case insensitive")},
             {"max_results", num("Max result lines, default 200")}}, {"pattern"}),
        false, false, t_grep);
    add("shell", "Run a bash command in the working directory and return combined output and the exit code. Cannot be undone by snapshots.",
        obj({{"command", str("Command line")}, {"timeout_s", num("Timeout in seconds")}, {"cwd", str("Directory to run in")}}, {"command"}),
        false, true, t_shell);
    add("web_search", "Search the web through the local SearXNG. Returns title, url and snippet for each result.",
        obj({{"query", str("Search query")}, {"max_results", num("Max results, default 8")}}, {"query"}), false, false, t_web_search);
    add("web_fetch", "Fetch a web page and return its text.",
        obj({{"url", str("http or https URL")}, {"max_chars", num("Max characters returned, default 20000")}}, {"url"}), false, false, t_web_fetch);
    add("snapshot_list", "List recent checkpoints (one per tool step) of this session with id, label, file count and reverted state.",
        obj({{"limit", num("Max checkpoints, default 20")}}, {}), false, false, t_snapshot_list);
    add("snapshot_diff", "Show the unified diff between a checkpoint's before-state and the current files.",
        obj({{"id", str("Checkpoint id")}}, {"id"}), false, false, t_snapshot_diff);
    add("snapshot_revert", "Restore the files a checkpoint changed. With to=true, also revert every newer checkpoint of this session.",
        obj({{"id", str("Checkpoint id")}, {"to", bln("Revert this and all newer checkpoints")}}, {"id"}), true, false, t_snapshot_revert);
}

std::string safety_floor_command(const std::string & cmd) { return blocked_command(cmd); }

std::string safety_floor_path(const ToolContext & ctx, const std::string & path) {
    fs::path resolved;
    return read_denied(ctx, path, resolved);
}

std::string truncate_output(const std::string & s, size_t max) { return cap_text(s, max); }

std::string find_bwrap() {
    if (const char * path = std::getenv("PATH")) {
        std::string ps = path;
        size_t a = 0;
        while (a <= ps.size()) {
            size_t b = ps.find(':', a);
            std::string dir = ps.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!dir.empty()) {
                fs::path c = fs::path(dir) / "bwrap";
                if (::access(c.c_str(), X_OK) == 0) return c.string();
            }
            if (b == std::string::npos) break;
            a = b + 1;
        }
    }
    return ::access("/usr/bin/bwrap", X_OK) == 0 ? "/usr/bin/bwrap" : "";
}

}  // namespace judas
