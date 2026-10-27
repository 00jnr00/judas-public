// Curator implementation. The planning is pure (plan_transitions); run_curator does the file work: backup,
// state file, archive moves, usage state switches and the report. No LLM pass in this version.
#include "learning/curator.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "core/json.h"

namespace judas {
namespace fs = std::filesystem;

namespace {

constexpr long long kDayMs = 86'400'000LL;

std::string stamp_for(long long ms) {
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm lt{};
    localtime_r(&t, &lt);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &lt);
    return buf;
}

std::string state_path(const std::string & data_dir) { return (fs::path(data_dir) / "curator.json").string(); }

long long day_count(long long idle_ms) { return idle_ms / kDayMs; }

std::string state_summary(const std::vector<CuratorTransition> & ts) {
    if (ts.empty()) return "no transitions";
    static const char * names[] = {"archive", "stale", "activate", "seed"};
    std::ostringstream o;
    for (const char * a : names) {
        int n = 0;
        for (const auto & t : ts) if (t.action == a) n++;
        if (n) {
            if (!o.str().empty()) o << ", ";
            o << a << " " << n;
        }
    }
    return o.str();
}

// "skill's" quoting for one shell word, the only metacharacter inside single quotes.
std::string shq(const std::string & path) {
    std::string s = "'";
    for (char c : path) {
        if (c == '\'') s += "'\\''";
        else s += c;
    }
    return s + "'";
}

}  // namespace

CuratorPlan plan_transitions(const std::vector<Skill> & skills, const std::vector<SkillUsageRow> & rows,
                             long long now, const LearningConfig & cfg) {
    std::map<std::string, const SkillUsageRow *> by_name;
    for (const auto & r : rows) by_name[r.name] = &r;
    CuratorPlan plan;
    // Only agent-created skills are curated: skill_manage wrote them, so Heartbeat-style ownership is
    // explicit in the frontmatter. Pinned and every other source are user owned; neither is touched.
    for (const auto & s : skills) {
        if (s.pinned || s.source != "agent") continue;
        auto it = by_name.find(s.name);
        if (it == by_name.end()) {
            plan.transitions.push_back({s.name, "seed", "no usage record yet: seed created_ms = now and defer"});
            continue;
        }
        const SkillUsageRow & r = *it->second;
        long long anchor = r.last_used_ms > 0 ? r.last_used_ms : r.created_ms;
        if (anchor <= 0) {
            plan.transitions.push_back({s.name, "seed", "record has no anchor: seed created_ms = now and defer"});
            continue;
        }
        long long idle = now - anchor;
        if (idle >= cfg.archive_after_days * kDayMs) {
            plan.transitions.push_back(
                {s.name, "archive", "unused " + std::to_string(day_count(idle)) + " days, past archive_after_days "
                                            + std::to_string(cfg.archive_after_days)});
        } else if (idle >= cfg.stale_after_days * kDayMs) {
            if (r.state != "stale")  // already stale and still idle: nothing to change
                plan.transitions.push_back(
                    {s.name, "stale", "unused " + std::to_string(day_count(idle)) + " days, past stale_after_days "
                                              + std::to_string(cfg.stale_after_days)});
        } else if (r.state == "stale") {
            plan.transitions.push_back({s.name, "activate", "used again, recorded anchor inside the stale window"});
        }
    }
    return plan;
}

std::optional<CuratorState> read_curator_state(const std::string & data_dir) {
    try {
        std::ifstream in(state_path(data_dir));
        if (!in) return std::nullopt;
        json j = json::parse(in);
        CuratorState s;
        s.last_run_ms = j.value("last_run_ms", 0LL);
        s.run_count = std::max(0, j.value("run_count", 0));
        s.summary = j.value("last_summary", std::string());
        return s;
    } catch (...) {
        return std::nullopt;  // corrupt state = nothing ran yet; the next pass seeds it again
    }
}

void write_curator_state(const std::string & data_dir, const CuratorState & s) {
    fs::create_directories(data_dir);
    std::ofstream out(state_path(data_dir));
    out << json{{"last_run_ms", s.last_run_ms},
                {"run_count", s.run_count},
                {"last_summary", s.summary}}.dump();
}

namespace {

// tar the skills dir into <data_dir>/skill-backups/skills-<stamp>.tar.gz and keep only the `keep` newest
// (name sorting is chronological). Returns false when the snapshot did not land: the whole pass then aborts
// before anything else touches the library.
bool backup_skills(const fs::path & data_dir, fs::path skills_dir, int keep, long long now, std::string & err) {
    std::error_code ec;
    if (skills_dir.empty()) { err = "no skills dir to back up"; return false; }
    skills_dir = fs::absolute(skills_dir, ec);
    if (!fs::is_directory(skills_dir, ec)) { err = "skills dir missing: " + skills_dir.string(); return false; }
    fs::path backups = data_dir / "skill-backups";
    fs::create_directories(backups, ec);
    fs::path dest = backups / ("skills-" + stamp_for(now) + ".tar.gz");
    // Members are stored relative to the parent, so a backup restores into the same shaped tree.
    std::string parent = skills_dir.parent_path().string();
    if (parent.empty()) parent = ".";
    std::string cmd = "tar -czf " + shq(dest.string()) + " -C " + shq(parent) + " " +
                      shq(skills_dir.filename().string());
    int rc = std::system(cmd.c_str());
    if (rc != 0 || !fs::exists(dest, ec)) {
        err = "skills backup failed (tar exit " + std::to_string(rc) + "): " + dest.string();
        return false;
    }
    // Prune older snapshots, oldest first.
    std::vector<fs::path> kept;
    for (const auto & e : fs::directory_iterator(backups, ec)) {
        std::string name = e.path().filename().string();
        if (name.rfind("skills-", 0) == 0 && name.size() > 7 && name.substr(name.size() - 7) == ".tar.gz")
            kept.push_back(e.path());
    }
    std::sort(kept.begin(), kept.end());
    for (size_t i = 0; kept.size() > (size_t) std::max(1, keep) && i < kept.size() - (size_t) std::max(1, keep); i++)
        fs::remove(kept[i], ec);
    return true;
}

// Copies one planned change onto the library. archive moves into <skill dir>/.archive/ (never deletes); a
// name collision renames with a stamp, so a crashed pass can be replayed idempotently. Returns a failure
// note for the report, empty on success.
std::string archive_one(const Skill & s, long long now, std::string & note) {
    fs::path src(s.path);
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        if (fs::exists(src.parent_path() / ".archive" / src.filename(), ec)) {
            note = "already archived by an earlier pass";
            return "";  // idempotent retry: the file is already where we wanted it
        }
        return "skill file is gone: " + src.string();
    }
    fs::path dir = src.parent_path() / ".archive";
    fs::create_directories(dir, ec);
    fs::path dest = dir / src.filename();
    if (fs::exists(dest, ec)) {
        fs::path stem = dest.stem(), ext = dest.extension();
        dest = dir / (stem.string() + "-" + stamp_for(now) + ext.string());
    }
    note = "moved to " + dest.string();
    fs::rename(src, dest, ec);
    if (ec) return "move failed: " + ec.message();
    return "";
}

}  // namespace

CuratorRunResult run_curator(const LearningConfig & cfg, const std::vector<Skill> & skills,
                             const std::string & data_dir, const std::string & skills_dir, SkillUsage & usage,
                             long long now, bool dry_run) {
    CuratorRunResult out;
    out.dry_run = dry_run;
    std::error_code ec;
    fs::create_directories(data_dir, ec);

    auto state = read_curator_state(data_dir);
    CuratorPlan plan = plan_transitions(skills, usage.all(), now, cfg);
    out.first_run = !state.has_value();

    // First run ever: seed the schedule and stop; the interval then elapses like for any run. A dry run on
    // a never-curated library is a pure preview: it does not seed because it does not write state.
    if (out.first_run) {
        if (dry_run) {
            out.summary = "first run (dry preview, nothing seeded): " + state_summary(plan.transitions);
            out.transitions = plan.transitions;
        } else {
            CuratorState seed;
            seed.last_run_ms = now;
            seed.run_count = 0;
            seed.summary = "first run: schedule seeded, nothing curated yet";
            write_curator_state(data_dir, seed);
            out.summary = seed.summary;
            out.report_path = "";
            return out;
        }
    }

    if (!dry_run) {
        std::string err;
        if (!backup_skills(fs::path(data_dir), fs::path(skills_dir), cfg.curator_backups, now, err)) {
            out.error = err;   // a failed backup aborts the run before anything moves
            return out;
        }
        // State before the moves: a crash here costs an idempotent replay on the next interval, never a
        // second archive of something already moved.
        CuratorState next = *state;
        next.last_run_ms = now;
        next.run_count += 1;
        next.summary = state_summary(plan.transitions);
        write_curator_state(data_dir, next);
        out.ran = true;   // from here on the run really happened (the moves below are its body)
    }

    std::map<std::string, const Skill *> by_name;
    for (const auto & s : skills) by_name[s.name] = &s;
    std::vector<CuratorTransition> applied;
    int failures = 0;
    if (!dry_run) {
        for (const auto & t : plan.transitions) {
            CuratorTransition done = t;
            auto it = by_name.find(t.name);
            if (t.action == "archive") {
                if (it == by_name.end()) {
                    done.reason += "; failed: skill is not in the loaded set";
                    failures++;
                } else {
                    std::string note;
                    std::string err = archive_one(*it->second, now, note);
                    if (!note.empty()) done.reason += "; " + note;
                    if (!err.empty()) { done.reason += "; failed: " + err; failures++; }
                }
            } else if (t.action == "seed") {
                usage.record_create(t.name);
            } else if (t.action == "stale" || t.action == "activate") {
                try {
                    usage.set_state(t.name, t.action == "activate" ? "active" : "stale");
                } catch (const std::exception & e) {
                    done.reason += "; failed: " + std::string(e.what());
                    failures++;
                }
            } else {
                done.reason += "; failed: unknown action";   // cannot happen today
                failures++;
            }
            applied.push_back(std::move(done));
        }
    }
    if (failures) {
        // The report keeps failed transitions alongside the good ones so nothing disappears silently.
        std::ostringstream o;
        o << "; " << failures << " failed";
        out.summary = (dry_run ? state_summary(plan.transitions) : state_summary(applied)) + o.str();
    } else {
        out.summary = dry_run ? state_summary(plan.transitions) : state_summary(applied);
    }
    out.transitions = dry_run ? plan.transitions : applied;

    // The report is written in both modes (a dry run writes only this).
    fs::path reports = fs::path(data_dir) / "curator";
    fs::create_directories(reports, ec);
    fs::path rep = reports / ("REPORT-" + stamp_for(now) + ".md");
    std::ofstream f(rep);
    f << "# Curator report " << stamp_for(now) << (dry_run ? " (dry run)\n" : "\n") << "\n"
      << "- run_count: " << (state ? std::to_string(state->run_count + (dry_run ? 0 : 1)) : "0")
      << (dry_run ? " (unchanged, dry run)" : "") << "\n"
      << "- summary: " << out.summary << "\n"
      << "- transitions:\n";
    if (out.transitions.empty()) f << "  - none\n";
    for (const auto & t : out.transitions)
        f << "  - " << t.name << ": " << t.action << "; " << t.reason << "\n";
    out.report_path = rep.string();
    return out;
}

}  // namespace judas
