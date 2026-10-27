// run_curator end to end in temp dirs: first-run seeding, backup, archive moves, state before applying,
// reports, dry runs and the abort-when-backup-fails rule. record_* stamps the wall clock, so usage rows are
// poked straight in SQL to line them up with the (fake) pass clocks.
#include <chrono>
#include <sqlite3.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "learning/curator.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

namespace {

constexpr long long day = 86'400'000LL;

// A small clock helper independent of the store header, so usage rows and pass clocks can line up.
long long wall_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

Skill sk(const std::string & name, const std::string & path = "", const std::string & source = "agent",
         bool pinned = false) {
    Skill s;
    s.name = name;
    s.path = path;
    s.source = source;
    s.pinned = pinned;
    return s;
}

SkillUsageRow row(const std::string & name, long long created, long long last_used,
                  const std::string & state = "active") {
    SkillUsageRow r;
    r.name = name;
    r.created_ms = created;
    r.last_used_ms = last_used;
    r.state = state;
    return r;
}

fs::path write_skill_file(const fs::path & dir, const std::string & name) {
    fs::path p = dir / (name + ".md");
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "---\nname: " << name << "\ndescription: " << name << "\n"
                     << "source: agent\nversion: 1\n---\n\nbody\n";
    return p;
}

std::string read_text(const fs::path & p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

LearningConfig cfg(int keep) {
    LearningConfig c;
    c.curator_backups = keep;
    return c;
}

// Set usage anchors directly: the public record_* calls stamp the real wall clock, which cannot stand in
// for "40 days ago".
void poke(const fs::path & db, const std::string & sql) {
    sqlite3 * raw = nullptr;
    REQUIRE(sqlite3_open_v2(db.string().c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
}

}  // namespace

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-curator-run-" + std::to_string(getpid());
    fs::remove_all(root);

    // ---- First run ever: only the state is seeded, nothing else happens. ----
    {
        fs::path data = root / "first", skills = root / "first-skills";
        fs::create_directories(skills);
        write_skill_file(skills, "fresh");
        SkillUsage u((data / "skills.db").string());
        Skill a = sk("fresh");
        long long t = wall_ms();
        auto r = run_curator(cfg(5), {a}, data.string(), skills.string(), u, t, false);
        CHECK(!r.ran && r.first_run);
        CHECK(r.error.empty() && r.report_path.empty());
        auto st = read_curator_state(data.string());
        REQUIRE(st.has_value());
        CHECK(st->last_run_ms == t);
        CHECK(st->run_count == 0);
        CHECK(st->summary.find("seeded") != std::string::npos);
        CHECK(!fs::exists(data / "curator"));
        CHECK(!fs::exists(data / "skill-backups"));
        CHECK(fs::exists(skills / "fresh.md"));  // the library was not touched
    }

    // ---- Real run: backup, archive move, seeds, report, pruning. ----
    {
        fs::path data = root / "real", skills = root / "real-skills";
        fs::create_directories(skills);
        fs::path gone = write_skill_file(skills, "forgotten");
        fs::path mine = write_skill_file(skills, "mine");
        fs::path loc = write_skill_file(skills, "localone");
        SkillUsage u((data / "skills.db").string());
        // forgotten: created at the wall clock, then the pass runs 100 days later (> archive_after_days).
        u.record_create("forgotten");
        Skill a = sk("forgotten", gone.string());
        Skill b = sk("mine", mine.string(), "agent", true);
        Skill c = sk("localone", loc.string(), "local");
        run_curator(cfg(5), {a, b, c}, data.string(), skills.string(), u, wall_ms(), false);  // seed schedule
        long long base = wall_ms();
        long long later = base + 100 * day;
        auto r = run_curator(cfg(2), {a, b, c}, data.string(), skills.string(), u, later, false);
        REQUIRE(r.error.empty());
        CHECK(r.ran && !r.first_run);

        // archived, never deleted; pinned and local files stay right where they are.
        CHECK(!fs::exists(gone));
        CHECK(read_text(skills / ".archive" / "forgotten.md").find("forgotten") != std::string::npos);
        CHECK(fs::exists(mine) && fs::exists(loc));

        auto st = read_curator_state(data.string());
        REQUIRE(st.has_value());
        CHECK(st->last_run_ms == later);
        CHECK(st->run_count == 1);

        // One report exists and names the archive move; no report for the seeding pass.
        int report_count = 0;
        bool has_line = false;
        for (const auto & p : fs::directory_iterator(data / "curator")) {
            if (p.path().filename().string().rfind("REPORT-", 0) == 0) {
                report_count++;
                has_line = read_text(p.path()).find("forgotten: archive") != std::string::npos;
            }
        }
        CHECK(report_count == 1 && has_line);

        // The backup is a real gzip tar in skill-backups; pruning keeps the newest cfg.curator_backups.
        fs::path backups = data / "skill-backups";
        auto tar_files = [&] {
            std::vector<std::string> names;
            for (const auto & p : fs::directory_iterator(backups))
                if (p.path().extension() == ".gz") names.push_back(p.path().filename().string());
            std::sort(names.begin(), names.end());
            return names;
        };
        CHECK(tar_files().size() == 1);
        run_curator(cfg(2), {a, b, c}, data.string(), skills.string(), u, later + day, false);
        CHECK(tar_files().size() == 2);
        run_curator(cfg(2), {a, b, c}, data.string(), skills.string(), u, later + 2 * day, false);
        CHECK(tar_files().size() == 2);   // oldest of three pruned
        CHECK(!fs::exists(gone) && fs::exists(skills / ".archive" / "forgotten.md"));  // nothing else moved
    }

    // ---- State flips and seeding inside a real pass. ----
    {
        fs::path data = root / "states", skills = root / "states-skills";
        fs::create_directories(skills);
        write_skill_file(skills, "cooling");
        write_skill_file(skills, "revived");
        write_skill_file(skills, "brand");
        SkillUsage u((data / "skills.db").string());
        long long base = wall_ms();
        u.record_create("cooling");        // real row so the update below has something to poke
        u.record_load("revived", false);
        Skill b = sk("cooling", (skills / "cooling.md").string());
        Skill c = sk("revived", (skills / "revived.md").string());
        Skill d = sk("brand", (skills / "brand.md").string());
        long long later = base + 40 * day;
        poke(data / "skills.db",
             "UPDATE skill_usage SET created_ms = " + std::to_string(base) +
                 " WHERE name = 'cooling';"
                 "UPDATE skill_usage SET last_used_ms = " + std::to_string(later - 5 * day) +
                 ", created_ms = " + std::to_string(base) + ", state = 'stale' WHERE name = 'revived';");
        run_curator(cfg(5), {b, c, d}, data.string(), skills.string(), u, base, false);   // seed schedule
        auto r = run_curator(cfg(5), {b, c, d}, data.string(), skills.string(), u, later, false);
        REQUIRE(r.error.empty());
        CHECK(r.ran);
        CHECK(u.get("cooling")->state == "stale");     // past stale_after_days, never used
        CHECK(u.get("revived")->state == "active");    // used again inside the stale window: reactivated
        // brand had no record: seeded at the pass clock and never touched otherwise.
        auto brand = u.get("brand");
        REQUIRE(brand.has_value());
        CHECK(brand->created_ms >= base);
        CHECK(brand->loads == 0 && brand->patches == 0 && brand->last_used_ms == 0);
        CHECK(r.summary == "stale 1, activate 1, seed 1");
    }

    // ---- Dry run: plan and report only. ----
    {
        fs::path data = root / "dry", skills = root / "dry-skills";
        fs::create_directories(skills);
        fs::path old = write_skill_file(skills, "oldman");
        SkillUsage u((data / "skills.db").string());
        Skill a = sk("oldman", old.string());
        u.record_create("oldman");
        run_curator(cfg(5), {a}, data.string(), skills.string(), u, wall_ms(), false);  // seed schedule
        u.set_state("oldman", "stale");
        auto pre = *read_curator_state(data.string());

        auto r = run_curator(cfg(5), {a}, data.string(), skills.string(), u, wall_ms() + 110 * day, true);
        CHECK(r.dry_run);
        CHECK(!r.report_path.empty());
        CHECK(fs::exists(old));                    // nothing moved
        CHECK(u.get("oldman")->state == "stale");  // nothing switched
        auto st = read_curator_state(data.string());
        REQUIRE(st.has_value());
        CHECK(st->run_count == pre.run_count);     // counters untouched
        CHECK(st->last_run_ms == pre.last_run_ms);
        CHECK(!fs::exists(data / "skill-backups"));  // no backup either: a dry run writes only the report
        CHECK(fs::exists(r.report_path));
        CHECK(read_text(r.report_path).find("(dry run)") != std::string::npos);
        CHECK(read_text(r.report_path).find("oldman: archive") != std::string::npos);
    }

    // ---- A dry run on a never-curated library is a preview, not a seed. ----
    {
        fs::path data = root / "dryfirst", skills = root / "dryfirst-skills";
        fs::create_directories(skills);
        write_skill_file(skills, "preview");
        SkillUsage u((data / "skills.db").string());
        Skill a = sk("preview", (skills / "preview.md").string());
        auto r = run_curator(cfg(5), {a}, data.string(), skills.string(), u, wall_ms(), true);
        CHECK(!read_curator_state(data.string()).has_value());  // no state written
        CHECK(u.get("preview") == std::nullopt);                // nothing seeded
        CHECK(fs::exists(skills / "preview.md"));
        CHECK(fs::exists(r.report_path));
    }

    // ---- A failed backup aborts the run before the state or the moves change. ----
    {
        fs::path data = root / "fail";
        fs::create_directories(data);
        SkillUsage u((data / "skills.db").string());
        Skill a = sk("ghost");
        long long t = wall_ms();
        run_curator(cfg(5), {a}, data.string(), (data / "no-such-skills").string(), u, t, false);
        auto pre = *read_curator_state(data.string());
        auto r = run_curator(cfg(5), {a}, data.string(), (data / "no-such-skills").string(), u,
                             t + day, false);
        CHECK(!r.ran);
        CHECK(r.error.find("backup") != std::string::npos || r.error.find("missing") != std::string::npos);
        auto st = read_curator_state(data.string());
        REQUIRE(st.has_value());
        CHECK(st->run_count == pre.run_count);
        CHECK(!fs::exists(data / "curator"));
    }

    // ---- A second archive pass into a name collision lands as a stamped copy. ----
    {
        fs::path data = root / "collide", skills = root / "collide-skills";
        fs::create_directories(skills / ".archive");
        write_skill_file(skills / ".archive", "forgotten");  // an earlier pass already filed one
        fs::path src = write_skill_file(skills, "forgotten");
        SkillUsage u((data / "skills.db").string());
        Skill a = sk("forgotten", src.string());
        u.record_create("forgotten");
        long long t = wall_ms();
        run_curator(cfg(5), {a}, data.string(), skills.string(), u, t, false);
        auto r = run_curator(cfg(5), {a}, data.string(), skills.string(), u, t + 100 * day, false);
        CHECK(r.ran && r.error.empty());
        CHECK(!fs::exists(src));                                   // moved away
        CHECK(fs::exists(skills / ".archive" / "forgotten.md"));   // old copy untouched
        int stamped = 0;
        for (const auto & p : fs::directory_iterator(skills / ".archive"))
            if (p.path().filename().string().rfind("forgotten-20", 0) == 0) stamped++;
        CHECK(stamped == 1);
        CHECK(read_text(r.report_path).find("forgotten: archive") != std::string::npos);
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
