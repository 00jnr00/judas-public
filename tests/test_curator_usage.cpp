// Curator on SkillUsage: the state column (active | stale), set_state, and the migration that adds the
// column to skills.db files created before the curator joined.
#include <sqlite3.h>
#include <unistd.h>

#include <filesystem>
#include <stdexcept>
#include <vector>

#include "skills/usage.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

namespace {

// Build a skills.db with the pre-curator schema by hand, the way an old build left it.
void write_pre_curator_db(const fs::path & db) {
    sqlite3 * raw = nullptr;
    REQUIRE(sqlite3_open_v2(db.string().c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr)
            == SQLITE_OK);
    REQUIRE(sqlite3_exec(raw,
                         "CREATE TABLE skill_usage ("
                         "  name TEXT PRIMARY KEY, loads INTEGER NOT NULL DEFAULT 0,"
                         "  autoloads INTEGER NOT NULL DEFAULT 0, patches INTEGER NOT NULL DEFAULT 0,"
                         "  created_ms INTEGER NOT NULL, last_used_ms INTEGER NOT NULL DEFAULT 0);"
                         "INSERT INTO skill_usage(name, loads, created_ms) VALUES('old', 4, 1000);"
                         "INSERT INTO skill_usage(name, created_ms) VALUES('fresh', 2000);",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);
}

}  // namespace

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-curator-usage-" + std::to_string(getpid());
    fs::remove_all(root);

    // set_state persists and keeps counters untouched.
    {
        fs::path db = root / "a" / "skills.db";
        SkillUsage u(db.string());
        u.record_load("alpha", true);
        u.record_patch("alpha");
        CHECK(u.get("alpha")->state == "active");

        u.set_state("alpha", "stale");
        auto a = u.get("alpha");
        REQUIRE(a.has_value());
        CHECK(a->state == "stale");
        CHECK(a->loads == 1 && a->patches == 1);

        u.set_state("beta", "stale");
        auto b = u.get("beta");
        REQUIRE(b.has_value());          // the stale call created the record
        CHECK(b->state == "stale");
        CHECK(b->created_ms > 0 && b->last_used_ms == 0);

        u.set_state("beta", "active");
        CHECK(u.get("beta")->state == "active");
        CHECK(u.all().size() == 2);

        bool threw = false;
        try {
            u.set_state("alpha", "archived");   // not a valid column value
        } catch (const std::exception &) {
            threw = true;
        }
        CHECK(threw);
        CHECK(u.get("alpha")->state == "stale");  // the bad call left nothing behind
    }

    // A second handle sees the persisted state.
    {
        fs::path db = root / "a" / "skills.db";
        SkillUsage u(db.string());
        CHECK(u.get("alpha")->state == "stale");
    }

    // Migration: a pre-state database gains the column and keeps its rows.
    fs::path old = root / "b" / "skills.db";
    fs::create_directories(old.parent_path());
    write_pre_curator_db(old);
    {
        SkillUsage u(old.string());
        auto o = u.get("old");
        REQUIRE(o.has_value());
        CHECK(o->loads == 4);
        CHECK(o->created_ms == 1000);
        CHECK(o->state == "active");     // everything recorded pre-curator counts as active
        CHECK(u.get("fresh")->state == "active");

        u.set_state("old", "stale");
        CHECK(u.get("old")->state == "stale");
        CHECK(u.get("old")->loads == 4);  // the migration did not reset counters

        SkillUsage u2(old.string());      // reopen: the column now exists and stays
        CHECK(u2.get("old")->state == "stale");
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
