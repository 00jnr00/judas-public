// Skill usage: counters, ordering, persistence and thread safety in a temp database.
#include <unistd.h>

#include <filesystem>
#include <thread>
#include <vector>

#include "skills/usage.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-skills-usage-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::path db = root / "nested" / "data" / "skills.db";

    {
        SkillUsage u(db.string());
        CHECK(u.all().empty());
        CHECK(!u.get("alpha").has_value());

        u.record_load("alpha", false);
        auto a = u.get("alpha");
        REQUIRE(a.has_value());
        CHECK(a->loads == 1);
        CHECK(a->autoloads == 0);
        CHECK(a->patches == 0);
        CHECK(a->created_ms > 0);
        CHECK(a->last_used_ms > 0);

        u.record_load("alpha", true);
        u.record_load("alpha", false);
        a = u.get("alpha");
        REQUIRE(a.has_value());
        CHECK(a->loads == 3);
        CHECK(a->autoloads == 1);

        u.record_patch("alpha");
        u.record_patch("alpha");
        a = u.get("alpha");
        REQUIRE(a.has_value());
        CHECK(a->patches == 2);

        u.record_create("beta");
        auto b = u.get("beta");
        REQUIRE(b.has_value());
        CHECK(b->loads == 0 && b->autoloads == 0 && b->patches == 0);
        CHECK(b->created_ms > 0);
        CHECK(b->last_used_ms == 0);

        auto rows = u.all();
        REQUIRE(rows.size() == 2);
        CHECK(rows[0].name == "alpha");
        CHECK(rows[1].name == "beta");
    }

    // A second handle sees the persisted rows.
    {
        SkillUsage u(db.string());
        auto a = u.get("alpha");
        REQUIRE(a.has_value());
        CHECK(a->loads == 3 && a->autoloads == 1 && a->patches == 2);
        CHECK(u.get("missing") == std::nullopt);
    }

    // Concurrent loads do not lose increments.
    {
        SkillUsage u(db.string());
        const int threads = 4, per_thread = 50;
        std::vector<std::thread> ts;
        for (int t = 0; t < threads; t++)
            ts.emplace_back([&, t] {
                for (int i = 0; i < per_thread; i++) u.record_load("gamma", t % 2 == 0);
            });
        for (auto & t : ts) t.join();
        auto g = u.get("gamma");
        REQUIRE(g.has_value());
        CHECK(g->loads == threads * per_thread);
        CHECK(g->autoloads == threads / 2 * per_thread);
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
