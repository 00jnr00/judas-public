// Heartbeat collectors and report assembly. Everything here is read-only: it reads /proc and /sys, stats
// paths, runs systemctl show / user-supplied read-only commands, and GETs URLs.
#include "heartbeat/heartbeat.h"

#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <fstream>
#include <sstream>
#include <thread>

#include "cpp-httplib/httplib.h"

namespace judas {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Proc {
    int code = -1;
    bool timed_out = false;
    bool start_failed = false;
    std::string out;
};

// fork/exec with a pipe; the child leads its own process group so a timeout kills the whole tree.
Proc run_proc(const std::vector<std::string> & argv, int timeout_s, size_t cap) {
    Proc r;
    int fds[2];
    if (pipe(fds) != 0) { r.start_failed = true; r.out = "pipe failed"; return r; }
    std::vector<char *> av;
    for (auto & a : argv) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); r.start_failed = true; r.out = "fork failed"; return r; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) dup2(dn, 2);
        int in = open("/dev/null", O_RDONLY);
        if (in >= 0) dup2(in, 0);
        close(fds[0]); close(fds[1]);
        execvp(av[0], av.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    int fd = fds[0];
    auto deadline = Clock::now() + std::chrono::seconds(timeout_s);
    char buf[4096];
    int status = 0;
    bool exited = false;
    while (true) {
        struct pollfd pf{fd, POLLIN, 0};
        int pr = poll(&pf, 1, 100);
        if (pr > 0) {
            ssize_t n = read(fd, buf, sizeof buf);
            if (n > 0) { if (r.out.size() < cap) r.out.append(buf, std::min<size_t>(n, cap - r.out.size())); continue; }
            if (n == 0) break;
        }
        if (!exited && waitpid(pid, &status, WNOHANG) == pid) exited = true;
        if (exited && pr <= 0) break;
        if (Clock::now() > deadline) {
            r.timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
    }
    close(fd);
    if (!exited) waitpid(pid, &status, 0);
    if (r.timed_out) r.code = -1;
    else if (WIFEXITED(status)) r.code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) r.code = 128 + WTERMSIG(status);
    return r;
}

std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char) s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char) s[i])) i++;
    return s.substr(i);
}

std::vector<std::string> split(const std::string & s, char d) {
    std::vector<std::string> v;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, d)) { t = trim(t); if (!t.empty()) v.push_back(t); }
    return v;
}

std::string fmt1(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

bool read_file(const std::string & p, std::string & out) {
    std::ifstream f(p);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string age_text(long long s) {
    if (s < 0) s = 0;
    if (s < 120) return std::to_string(s) + "s";
    if (s < 7200) return std::to_string(s / 60) + "m";
    if (s < 172800) return std::to_string(s / 3600) + "h";
    return std::to_string(s / 86400) + "d";
}

// Each collector fills text (and optional structured hints); throwing sets the error.
using Fn = std::function<void(const std::string & arg, json & f)>;

void c_units(const std::string & arg, json & f) {
    std::vector<std::string> pats = split(arg, ',');
    if (pats.empty()) pats = {"judas*"};
    std::vector<std::string> names;
    for (auto & p : pats) {
        if (p.find_first_of("*?[") == std::string::npos) { names.push_back(p); continue; }
        Proc r = run_proc({"systemctl", "--user", "list-units", "--all", "--plain", "--no-legend", "--no-pager", p}, 10, 16384);
        if (r.timed_out || r.start_failed) throw std::runtime_error("systemctl list-units failed");
        std::stringstream ss(r.out);
        std::string line;
        while (std::getline(ss, line)) {
            std::stringstream ls(line);
            std::string n;
            ls >> n;
            if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        }
    }
    if (names.empty()) { f["text"] = "no matching units"; return; }
    std::vector<std::string> argv = {"systemctl", "--user", "show", "-p", "Id,ActiveState,SubState,NRestarts", "--no-pager"};
    for (auto & n : names) argv.push_back(n);
    Proc r = run_proc(argv, 10, 65536);
    if (r.timed_out || r.start_failed) throw std::runtime_error("systemctl show failed");
    std::ostringstream o;
    int failed = 0, active = 0, total = 0;
    std::stringstream ss(r.out);
    std::string line, id, act, sub, nr;
    auto flush = [&]() {
        if (id.empty()) return;
        total++;
        if (act == "failed") failed++;
        if (act == "active") active++;
        o << id << ": " << act << "/" << sub;
        if (!nr.empty() && nr != "0") o << " restarts=" << nr;
        o << "\n";
        id.clear(); act.clear(); sub.clear(); nr.clear();
    };
    while (std::getline(ss, line)) {
        if (line.empty()) { flush(); continue; }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "Id") id = v; else if (k == "ActiveState") act = v;
        else if (k == "SubState") sub = v; else if (k == "NRestarts") nr = v;
    }
    flush();
    f["text"] = trim(o.str());
    f["failed"] = failed > 0;
    f["units"] = {{"total", total}, {"active", active}, {"failed", failed}};
}

void c_http(const std::string & arg, json & f) {
    if (arg.rfind("http://", 0) != 0 && arg.rfind("https://", 0) != 0) throw std::runtime_error("http collector needs an http(s) URL");
    size_t se = arg.find("://") + 3;
    size_t ps = arg.find('/', se);
    std::string base = ps == std::string::npos ? arg : arg.substr(0, ps);
    std::string path = ps == std::string::npos ? "/" : arg.substr(ps);
    auto t0 = Clock::now();
    httplib::Client cli(base);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(5);
    cli.set_write_timeout(5);
    cli.enable_server_certificate_verification(false);
    auto res = cli.Get(path);
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    if (!res) {
        f["down"] = true;
        f["text"] = "DOWN " + arg + " (no response, " + std::to_string(ms) + " ms)";
        throw std::runtime_error("down: no response from " + arg);
    }
    bool up = res->status < 500;
    f["status"] = res->status;
    f["down"] = !up;
    f["text"] = std::string(up ? "UP " : "DOWN ") + arg + " HTTP " + std::to_string(res->status) + " (" + std::to_string(ms) + " ms)";
}

void c_cpu(const std::string &, json & f) {
    std::string s;
    if (!read_file("/proc/loadavg", s)) throw std::runtime_error("cannot read /proc/loadavg");
    std::stringstream ss(s);
    double a = 0, b = 0, c = 0;
    ss >> a >> b >> c;
    unsigned n = std::thread::hardware_concurrency();
    f["text"] = "load " + fmt1(a) + " " + fmt1(b) + " " + fmt1(c) + " (1/5/15 min), " + std::to_string(n) + " cores";
    f["load1"] = a;
    f["cores"] = n;
}

void c_ram(const std::string &, json & f) {
    std::string s;
    if (!read_file("/proc/meminfo", s)) throw std::runtime_error("cannot read /proc/meminfo");
    std::stringstream ss(s);
    std::string line;
    double total = 0, avail = 0;
    while (std::getline(ss, line)) {
        std::stringstream ls(line);
        std::string k;
        double kb = 0;
        ls >> k >> kb;
        if (k == "MemTotal:") total = kb;
        else if (k == "MemAvailable:") avail = kb;
    }
    if (total <= 0) throw std::runtime_error("MemTotal not found");
    double g = 1024.0 * 1024.0;
    f["text"] = "total " + fmt1(total / g) + " GB, used " + fmt1((total - avail) / g) + " GB, free " + fmt1(avail / g) + " GB";
    f["used_pct"] = std::round((total - avail) / total * 1000) / 10;
}

void c_gpu(const std::string &, json & f) {
    const std::string d = "/sys/class/drm/card1/device/";
    std::string used, total, mclk;
    if (!read_file(d + "mem_info_vram_used", used) || !read_file(d + "mem_info_vram_total", total))
        throw std::runtime_error("cannot read " + d + "mem_info_vram_*");
    double u = std::stod(trim(used)) / 1048576.0, t = std::stod(trim(total)) / 1048576.0;
    std::string text = "VRAM " + std::to_string((long long) u) + "/" + std::to_string((long long) t) + " MiB";
    if (read_file(d + "pp_dpm_mclk", mclk)) {
        std::stringstream ss(mclk);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find('*') == std::string::npos) continue;
            // e.g. "1: 96Mhz *"
            auto c = line.find(':');
            double mhz = std::atof(line.c_str() + (c == std::string::npos ? 0 : c + 1));
            f["mclk_mhz"] = mhz;
            text += ", mclk " + std::to_string((long long) mhz) + " MHz";
            break;
        }
    }
    f["text"] = text;
}

void c_disk(const std::string & arg, json & f) {
    std::string p = arg.empty() ? "/" : arg;
    std::error_code ec;
    auto sp = fs::space(p, ec);
    if (ec) throw std::runtime_error("disk " + p + ": " + ec.message());
    double g = 1024.0 * 1024.0 * 1024.0;
    double total = sp.capacity / g, avail = sp.available / g;
    double used = (sp.capacity - sp.free) / g;
    double pct = sp.capacity ? (double) (sp.capacity - sp.available) / sp.capacity * 100.0 : 0;
    f["text"] = p + ": total " + fmt1(total) + " GB, used " + fmt1(used) + " GB, free " + fmt1(avail) + " GB, " + fmt1(pct) + "% used";
    f["used_pct"] = std::round(pct * 10) / 10;
}

void c_dirgrowth(const std::string & arg, json & f) {
    if (arg.empty()) throw std::runtime_error("dirgrowth needs a path");
    std::error_code ec;
    if (!fs::exists(arg, ec)) throw std::runtime_error("no such path: " + arg);
    auto deadline = Clock::now() + std::chrono::seconds(4);
    unsigned long long bytes = 0, files = 0;
    bool capped = false;
    fs::recursive_directory_iterator it(arg, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (Clock::now() > deadline) { capped = true; break; }
        std::error_code e2;
        if (it->is_regular_file(e2) && !it->is_symlink(e2)) { bytes += it->file_size(e2); files++; }
    }
    f["text"] = arg + ": " + fmt1(bytes / 1048576.0) + " MB in " + std::to_string(files) + " files" + (capped ? " (walk capped at 4s, partial)" : "");
    f["mb"] = std::round(bytes / 1048576.0 * 10) / 10;
}

void c_backups(const std::string & arg, json & f) {
    if (arg.empty()) throw std::runtime_error("backups needs a path or glob");
    std::string dir = arg, pat = "*";
    std::error_code ec;
    if (!fs::is_directory(arg, ec)) {
        fs::path p(arg);
        dir = p.parent_path().empty() ? "." : p.parent_path().string();
        pat = p.filename().string();
    }
    if (!fs::is_directory(dir, ec)) throw std::runtime_error("no such directory: " + dir);
    fs::file_time_type newest{};
    std::string newest_name;
    int n = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (fnmatch(pat.c_str(), it->path().filename().c_str(), 0) != 0) continue;
        std::error_code e2;
        auto t = it->last_write_time(e2);
        if (e2) continue;
        n++;
        if (n == 1 || t > newest) { newest = t; newest_name = it->path().filename().string(); }
    }
    if (n == 0) throw std::runtime_error("no backups found in " + arg);
    auto age = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - newest).count();
    std::time_t wall = std::time(nullptr) - (std::time_t) age;
    char tb[40];
    std::strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", std::localtime(&wall));
    f["text"] = "newest " + newest_name + " at " + tb + " (" + age_text(age) + " ago), " + std::to_string(n) + " entries";
    f["age_s"] = (long long) age;
}

void c_command(const std::string & arg, json & f) {
    if (arg.empty()) throw std::runtime_error("command collector needs a command");
    Proc r = run_proc({"/bin/bash", "-lc", arg}, 15, 4096);
    std::string out = trim(r.out);
    f["text"] = out;
    if (r.start_failed) throw std::runtime_error(r.out);
    if (r.timed_out) throw std::runtime_error("timed out after 15s");
    if (r.code != 0) throw std::runtime_error("exit code " + std::to_string(r.code));
}

}  // namespace

json collect_facts(const HeartbeatConfig & cfg) {
    json facts = json::object();
    for (auto & c : cfg.collectors) {
        json f = json::object();
        auto t0 = Clock::now();
        try {
            Fn fn;
            if (c.kind == "units") fn = c_units;
            else if (c.kind == "http") fn = c_http;
            else if (c.kind == "cpu") fn = c_cpu;
            else if (c.kind == "ram") fn = c_ram;
            else if (c.kind == "gpu") fn = c_gpu;
            else if (c.kind == "disk") fn = c_disk;
            else if (c.kind == "dirgrowth") fn = c_dirgrowth;
            else if (c.kind == "backups") fn = c_backups;
            else if (c.kind == "command") fn = c_command;
            if (!fn) {
                f["error"] = "unknown collector kind: " + c.kind;
            } else {
                try { fn(c.arg, f); }
                catch (const std::exception & e) { f["error"] = e.what(); }
            }
        } catch (...) {
            f["error"] = "collector failed";
        }
        f["ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (!f.contains("text")) f["text"] = "";
        facts[c.name] = f;
    }
    return facts;
}

std::string facts_to_text(const json & facts) {
    std::ostringstream o;
    if (!facts.is_object()) return "";
    for (auto it = facts.begin(); it != facts.end(); ++it) {
        const json & f = it.value();
        o << "## " << it.key() << "\n";
        std::string text = f.is_object() ? f.value("text", std::string()) : std::string();
        if (f.is_object() && f.contains("error") && f["error"].is_string()) {
            o << "ERROR: " << f["error"].get<std::string>();
            if (!text.empty()) o << " | " << text;
            o << "\n";
        } else {
            o << text << "\n";
        }
    }
    return o.str();
}

namespace {
// Severity from the facts alone, used when the summarizer gives none of ok|notice|warn|critical.
std::string infer_severity(const json & facts) {
    bool crit = false, warn = false;
    for (auto & [name, f] : facts.items()) {
        if (!f.is_object()) continue;
        if (f.contains("error")) crit = true;
        if (f.value("down", false) || f.value("failed", false)) crit = true;
        if (f.contains("used_pct") && f["used_pct"].is_number() && f.contains("text")) {
            // used_pct also exists on ram; only disk gates on it
            std::string t = f.value("text", std::string());
            if (t.find("GB, ") != std::string::npos && t.find("% used") != std::string::npos) {
                double p = f["used_pct"].get<double>();
                if (p > 95) crit = true; else if (p > 85) warn = true;
            }
        }
        if (f.contains("mclk_mhz") && f["mclk_mhz"].is_number() && f["mclk_mhz"].get<double>() < 200) warn = true;
    }
    return crit ? "critical" : warn ? "warn" : "ok";
}
}  // namespace

HeartbeatReport run_heartbeat(const HeartbeatConfig & cfg, const Summarizer & summarize) {
    HeartbeatReport rep;
    rep.ran_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    rep.facts = collect_facts(cfg);
    std::string text = facts_to_text(rep.facts);
    std::string sev;
    if (summarize) {
        try {
            auto r = summarize(text);
            rep.summary = r.first;
            sev = r.second;
        } catch (const std::exception & e) {
            rep.summary = std::string("summarizer failed: ") + e.what();
        }
    }
    std::transform(sev.begin(), sev.end(), sev.begin(), [](unsigned char c) { return std::tolower(c); });
    sev = trim(sev);
    bool valid = sev == "ok" || sev == "notice" || sev == "warn" || sev == "critical";
    rep.severity = valid ? sev : infer_severity(rep.facts);
    return rep;
}

}  // namespace judas
