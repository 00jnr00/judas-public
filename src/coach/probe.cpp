// Coach P1: probes. See probe.h.
#include "coach/probe.h"

#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <thread>

#include "skills/guard.h"

namespace judas {
namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool spit(const fs::path & p, const std::string & text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return (bool) f;
}

std::string trim_ws(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

std::string shq(const std::string & s) {
    std::string o = "'";
    for (char c : s) o += (c == '\'') ? "'\\''" : std::string(1, c);
    return o + "'";
}

bool plain_name(const std::string & n) {
    if (n.empty() || n.size() > 48) return false;
    for (char c : n)
        if (!(std::isalnum((unsigned char) c) || c == '-' || c == '_')) return false;
    return true;
}

}  // namespace

bool ProbeRun::passed() const { return check_exit >= 0 ? check_exit == 0 : status == "done"; }

std::string coach_probe_overlay(const Config & cfg, const CoachVariant & v, std::string * err) {
    std::string overlay = trim_ws(slurp(fs::path(v.dir) / "overlay.md")), tools = trim_ws(slurp(fs::path(v.dir) / "tools.md")), why;
    if (coach_forbidden(overlay + "\n" + tools, &why)) {
        if (err) *err = "overlay or tool notes touch the safety floor (" + why + "); not probed";
        return "";
    }
    if ((int) overlay.size() > cfg.coach.overlay_max_chars || (int) tools.size() > cfg.coach.tools_max_chars) {
        if (err) *err = "overlay or tool notes are over the size caps; not probed";
        return "";
    }
    std::string out;
    if (!overlay.empty()) out += overlay;
    if (!tools.empty()) out += std::string(out.empty() ? "" : "\n\n") + "Tool notes:\n" + tools;
    if (out.empty()) return "";
    return "Trial guidance (applies to this session only):\n\n" + out;
}

int coach_probe_stage_skills(const CoachVariant & v, const std::string & dst, std::vector<std::string> * skipped) {
    std::error_code ec;
    fs::path src = fs::path(v.dir) / "skills";
    if (!fs::is_directory(src, ec)) return 0;
    std::vector<fs::path> files;
    for (auto & e : fs::directory_iterator(src, ec))
        if (e.path().extension() == ".md") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    int n = 0;
    for (auto & f : files) {
        std::string name = f.stem().string(), text = slurp(f), why;
        if (!plain_name(name)) { if (skipped) skipped->push_back(name + ": invalid name"); continue; }
        if (coach_forbidden(text, &why)) { if (skipped) skipped->push_back(name + ": touches the safety floor (" + why + ")"); continue; }
        if (scan_skill_text(text).blocked()) { if (skipped) skipped->push_back(name + ": skill guard blocked it"); continue; }
        if (spit(fs::path(dst) / (name + ".md"), text)) n++;
    }
    return n;
}

bool coach_probe_copy_fixture(const std::string & src, const std::string & dst, std::string * err) {
    std::error_code ec;
    if (!fs::is_directory(src, ec)) { if (err) *err = "fixture is not a directory: " + src; return false; }
    fs::create_directories(fs::path(dst).parent_path(), ec);
    if (fs::exists(dst, ec)) { if (err) *err = "work dir already exists: " + dst; return false; }
    // cp -a keeps modes, links and timestamps; --reflink=auto is instant on btrfs and a plain copy elsewhere.
    std::string cmd = "cp -a --reflink=auto " + shq(src) + " " + shq(dst) + " 2>&1";
    FILE * p = popen(cmd.c_str(), "r");
    if (!p) { if (err) *err = "could not run cp"; return false; }
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof buf, p)) out += buf;
    int rc = pclose(p);
    if (rc != 0) { if (err) *err = "cp failed: " + trim_ws(out); return false; }
    return true;
}

int coach_probe_run_check(const std::string & cmd, const std::string & dir, int timeout_s, std::string * tail) {
    std::string full = "cd " + shq(dir) + " && timeout -k 5 " + std::to_string(std::max(1, timeout_s)) + " sh -c " + shq(cmd) + " 2>&1";
    FILE * p = popen(full.c_str(), "r");
    if (!p) { if (tail) *tail = "could not start the check"; return 127; }
    std::deque<std::string> lines;
    std::string cur;
    char buf[4096];
    while (fgets(buf, sizeof buf, p)) {
        cur += buf;
        if (!cur.empty() && cur.back() == '\n') {
            cur.pop_back();
            if (cur.size() > 400) cur.resize(400);
            lines.push_back(cur);
            if (lines.size() > 40) lines.pop_front();
            cur.clear();
        } else if (cur.size() > 100000) cur.clear();
    }
    if (!cur.empty()) { lines.push_back(cur.substr(0, 400)); if (lines.size() > 40) lines.pop_front(); }
    int rc = pclose(p);
    if (tail) {
        tail->clear();
        for (auto & l : lines) *tail += l + "\n";
    }
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : 128 + (WIFSIGNALED(rc) ? WTERMSIG(rc) : 0);
}

std::string coach_probe_work_root(const Config & cfg, const ProbeOptions & o, const std::string & id, std::string * err) {
    fs::path root;
    if (!o.work_root.empty()) root = expand_home(o.work_root);
    else {
        const char * xdg = std::getenv("XDG_CACHE_HOME");
        root = (xdg && *xdg) ? fs::path(xdg) : fs::path(expand_home("~/.cache"));
        root /= "judas/probes";
    }
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(root), ec);
    fs::path data = fs::weakly_canonical(fs::absolute(expand_home(cfg.data_dir)), ec);
    auto rel = abs.lexically_relative(data);
    if (!rel.empty() && *rel.begin() != "..") {
        if (err) *err = "probe work_root must be outside data_dir (tools cannot write there): " + abs.string();
        return "";
    }
    return (abs / id).string();
}

std::string coach_probe_verdict(const std::vector<ProbeRun> & runs, int k) {
    int nb = 0, nv = 0, pb = 0, pv = 0, cb = 0, cv = 0;
    for (auto & r : runs) {
        bool v = r.arm == "variant";
        (v ? nv : nb)++;
        if (r.passed()) (v ? pv : pb)++;
        (v ? cv : cb) += r.stats.tool_calls;
    }
    std::string verdict;
    if (!nb || !nv) verdict = "no clear difference";
    else {
        // compare pass rates exactly: pv/nv against pb/nb
        long long lhs = (long long) pv * nb, rhs = (long long) pb * nv;
        // The call-count tiebreak only means something when the arms actually passed; fewer calls from a run that
        // timed out or stalled is not efficiency.
        bool any_pass = pv > 0 && pb > 0;
        bool fewer_v = any_pass && cb > 0 && cv * 10 <= cb * 8, fewer_b = any_pass && cv > 0 && cb * 10 <= cv * 8;
        if (lhs > rhs) verdict = "variant better";
        else if (lhs < rhs) verdict = "base better";
        else if (fewer_v) verdict = "variant better";
        else if (fewer_b) verdict = "base better";
        else verdict = "no clear difference";
    }
    std::ostringstream o;
    o << verdict << " (pass " << pv << "/" << nv << " variant vs " << pb << "/" << nb << " base; tool calls " << cv << " vs " << cb << ")";
    if (k <= 1) o << "; low confidence, k=1";
    return o.str();
}

namespace {

std::string fmt1(double d) {
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << d;
    return o.str();
}

}  // namespace

std::string coach_probe_evidence_md(const std::string & id, const ProbeOptions & o, const std::vector<ProbeRun> & runs,
                                    const std::string & verdict) {
    std::ostringstream m;
    m << "# Probe evidence for " << id << "\n\nVerdict: " << verdict << "\n\nRounds: " << o.k
      << (o.check.empty() ? ", no check command (pass means the run finished)" : ", check: `" + o.check + "`") << "\n\n"
      << "| arm | round | status | check exit | wall s | tool calls | writes | longest read streak | nudges | tokens |\n"
      << "|---|---|---|---|---|---|---|---|---|---|\n";
    for (auto & r : runs)
        m << "| " << r.arm << " | " << r.round << " | " << r.status << " | " << (r.check_exit < 0 ? "-" : std::to_string(r.check_exit))
          << " | " << fmt1(r.wall_s) << " | " << r.stats.tool_calls << " | " << r.stats.writes << " | " << r.stats.longest_read_streak
          << " | " << r.stats.nudges << " | " << (r.stats.tokens_in + r.stats.tokens_out) << " |\n";
    m << "\nThe verdict is mechanical: higher pass rate wins; on an equal, nonzero pass rate, at least 20% fewer tool calls wins. "
         "One sample per arm is noise as much as signal.\n";
    return m.str();
}

json coach_probe_evidence_json(const std::string & id, const ProbeOptions & o, const std::vector<ProbeRun> & runs,
                               const std::string & verdict) {
    json rs = json::array();
    for (auto & r : runs)
        rs.push_back({{"arm", r.arm}, {"round", r.round}, {"session", r.session}, {"work", r.work}, {"status", r.status},
                      {"check_exit", r.check_exit}, {"check_tail", r.check_tail}, {"wall_s", r.wall_s},
                      {"tool_calls", r.stats.tool_calls}, {"writes", r.stats.writes},
                      {"longest_read_streak", r.stats.longest_read_streak}, {"nudges", r.stats.nudges},
                      {"tokens_in", r.stats.tokens_in}, {"tokens_out", r.stats.tokens_out}, {"passed", r.passed()},
                      {"signals", r.signals}});
    return {{"proposal", id}, {"verdict", verdict}, {"k", o.k}, {"check", o.check}, {"mode", o.mode}, {"runs", rs}};
}

std::string coach_probe_evidence_text(const Config & cfg, const std::string & id) {
    if (!coach_valid_id(id)) return "";
    return slurp(fs::path(coach_root(cfg)) / id / "probes" / "evidence.md");
}

ProbeResult coach_probe(const Config & cfg, AgentManager & agents, Store & store, const std::string & id,
                        const ProbeOptions & o, const std::atomic<bool> * cancel) {
    ProbeResult res;
    CoachVariant v;
    if (!coach_load(cfg, id, v)) { res.error = "no such proposal: " + id; return res; }
    if (trim_ws(o.task).empty()) { res.error = "probe needs a task"; return res; }
    if (o.k < 1 || o.k > 10) { res.error = "k must be between 1 and 10"; return res; }
    std::error_code ec;
    if (!fs::is_directory(o.fixture, ec)) { res.error = "fixture is not a directory: " + o.fixture; return res; }
    std::string err, overlay = coach_probe_overlay(cfg, v, &err);
    if (!err.empty()) { res.error = err; return res; }

    std::string wr_err, wroot = coach_probe_work_root(cfg, o, id, &wr_err);
    if (wroot.empty()) { res.error = wr_err; return res; }
    fs::path pdir = fs::path(v.dir) / "probes";
    fs::remove_all(pdir, ec);   // a re-run replaces the previous evidence
    fs::remove_all(wroot, ec);
    fs::create_directories(pdir, ec);
    res.dir = pdir.string();
    std::string skills_dir = (pdir / "skills").string();
    int staged = coach_probe_stage_skills(v, skills_dir);

    for (int round = 1; round <= o.k && !(cancel && cancel->load()); round++) {
        for (const char * arm : {"base", "variant"}) {
            if (cancel && cancel->load()) break;
            bool is_variant = std::string(arm) == "variant";
            ProbeRun run;
            run.arm = arm;
            run.round = round;
            fs::path work = fs::path(wroot) / (std::to_string(round) + "-" + arm) / "work";
            run.work = work.string();
            std::string cerr;
            if (!coach_probe_copy_fixture(o.fixture, run.work, &cerr)) { res.error = cerr; return res; }

            // "[probe]" title prefix: the store has no hidden flag.
            SessionRow s = store.create_session("[probe] " + id + " " + arm + " r" + std::to_string(round), run.work,
                                                store.get_session(v.session) ? store.get_session(v.session)->model : "", o.mode);
            run.session = s.id;
            AgentManager::SessionExtras x;
            if (is_variant) {
                x.overlay = overlay;
                if (staged > 0) x.skills_dir = skills_dir;
            }
            agents.set_session_extras(s.id, x);   // base gets an empty one: same treatment, no background review

            auto t0 = std::chrono::steady_clock::now();
            agents.submit(s.id, o.task, run.work);
            std::atomic<bool> done{false};
            std::thread waiter([&] { agents.wait_session(s.id); done = true; });
            bool timed_out = false;
            while (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (!timed_out && el > o.timeout_s) { timed_out = true; agents.cancel(s.id); }
                if (cancel && cancel->load() && !timed_out) { timed_out = true; agents.cancel(s.id); }
            }
            waiter.join();
            run.wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            run.status = timed_out ? "timeout" : agents.turn_info(s.id).status;
            agents.clear_session_extras(s.id);

            run.stats = coach_stats(store, s.id);
            run.signals = coach_signals(store, s.id);
            if (!o.check.empty()) run.check_exit = coach_probe_run_check(o.check, run.work, o.check_timeout_s, &run.check_tail);
            res.runs.push_back(std::move(run));
        }
    }
    if (res.runs.empty()) { res.error = cancel && cancel->load() ? "cancelled" : "nothing ran"; return res; }

    res.verdict = coach_probe_verdict(res.runs, o.k);
    spit(pdir / "evidence.md", coach_probe_evidence_md(id, o, res.runs, res.verdict));
    spit(pdir / "evidence.json", coach_probe_evidence_json(id, o, res.runs, res.verdict).dump(2) + "\n");
    coach_set_probe(cfg, id, res.verdict);
    res.ok = true;
    return res;
}

}  // namespace judas
