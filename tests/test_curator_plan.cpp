// plan_transitions: the pure decision table of the curator.
#include "learning/curator.h"
#include "test.h"

using namespace judas;

namespace {

constexpr long long day = 86'400'000LL;
const long long now = 1'800'000'000'000LL;   // fixed clock so the table reads deterministic

Skill sk(const std::string & name, const std::string & source = "agent", bool pinned = false) {
    Skill s;
    s.name = name;
    s.source = source;
    s.pinned = pinned;
    return s;
}

SkillUsageRow row(const std::string & name, long long created, long long last_used, const std::string & state = "active") {
    SkillUsageRow r;
    r.name = name;
    r.created_ms = created;
    r.last_used_ms = last_used;
    r.state = state;
    return r;
}

const CuratorTransition * find(const CuratorPlan & p, const std::string & name) {
    for (const auto & t : p.transitions) if (t.name == name) return &t;
    return nullptr;
}

}  // namespace

int main() {
    LearningConfig cfg;   // defaults: stale 30, archive 90

    // Fresh active agent skill: untouched.
    {
        CuratorPlan p = plan_transitions({sk("fresh")}, {row("fresh", now - day, now - day)}, now, cfg);
        CHECK(p.transitions.empty());
    }

    // Past stale, not past archive: stale (once).
    {
        CuratorPlan p = plan_transitions({sk("cool")}, {row("cool", now - 40 * day, 0)}, now, cfg);
        REQUIRE(p.transitions.size() == 1);
        CHECK(p.transitions[0].action == "stale");
        CHECK(p.transitions[0].reason.find("40 days") != std::string::npos);
    }

    // Already stale and still idle: no second transition.
    {
        CuratorPlan p = plan_transitions({sk("cool")}, {row("cool", now - 40 * day, 0, "stale")}, now, cfg);
        CHECK(p.transitions.empty());
    }

    // Past archive: archived, even when the row already says stale.
    {
        CuratorPlan p = plan_transitions({sk("oldy")}, {row("oldy", now - 120 * day, 0, "stale")}, now, cfg);
        REQUIRE(p.transitions.size() == 1);
        CHECK(p.transitions[0].action == "archive");
        CHECK(p.transitions[0].reason.find("120 days") != std::string::npos);
    }

    // The archive boundary itself counts: exactly archive_after_days without load goes.
    {
        CuratorPlan p = plan_transitions({sk("edge")}, {row("edge", now - 90 * day, 0)}, now, cfg);
        REQUIRE(p.transitions.size() == 1);
        CHECK(p.transitions[0].action == "archive");
    }

    // A stale skill used again goes back to active; last_used_ms is the anchor, not created_ms, so the
    // revival is judged on the fresh last use and never on the old creation date.
    {
        CuratorPlan p = plan_transitions({sk("back")}, {row("back", now - 100 * day, now - 5 * day, "stale")}, now, cfg);
        REQUIRE(p.transitions.size() == 1);
        CHECK(p.transitions[0].action == "activate");
        CuratorPlan q = plan_transitions({sk("back")}, {row("back", now - 100 * day, now - 5 * day, "active")}, now, cfg);
        CHECK(q.transitions.empty());
    }

    // Never-used agent skill: created at day one is the anchor, so past archive it goes.
    {
        CuratorPlan p = plan_transitions({sk("never")}, {row("never", now - 95 * day, 0)}, now, cfg);
        REQUIRE(p.transitions.size() == 1);
        CHECK(p.transitions[0].action == "archive");

        CuratorPlan q = plan_transitions({sk("never")}, {row("never", now - 45 * day, 0)}, now, cfg);
        REQUIRE(q.transitions.size() == 1);
        CHECK(q.transitions[0].action == "stale");

        CuratorPlan r = plan_transitions({sk("never")}, {row("never", now - 3 * day, 0)}, now, cfg);
        CHECK(r.transitions.empty());
    }

    // No usage record at all: seeded now and deferred to the next pass.
    {
        CuratorPlan p = plan_transitions({sk("newy")}, {}, now, cfg);
        REQUIRE(p.transitions.size() == 1);
        CHECK(p.transitions[0].action == "seed");
        CHECK(p.transitions[0].reason.find("seed") != std::string::npos);
    }

    // A record without any anchor (half-written record) defers the same way.
    {
        CuratorPlan p = plan_transitions({sk("zombie")}, {row("zombie", 0, 0)}, now, cfg);
        REQUIRE(p.transitions.size() == 1);
        CHECK(p.transitions[0].action == "seed");
    }

    // Pinned skills are never in the plan, even when far past the archive window.
    {
        CuratorPlan p = plan_transitions({sk("mine", "agent", true)}, {row("mine", now - 400 * day, 0)}, now, cfg);
        CHECK(p.transitions.empty());
    }

    // Every other source is user owned and untouched: local, external, anything else.
    {
        CuratorPlan p =
            plan_transitions({sk("loc", "local"), sk("ext", "external:local-rules")},
                             {row("loc", now - 400 * day, 0), row("ext", now - 400 * day, 0)}, now, cfg);
        CHECK(p.transitions.empty());
    }

    // The planner only sees loaded skills: usage rows for skills that left the dir produce nothing.
    {
        CuratorPlan p = plan_transitions({}, {row("gone", now - 400 * day, 0)}, now, cfg);
        CHECK(p.transitions.empty());
    }

    // Ordered, deterministic output that follows the input order.
    {
        CuratorPlan p = plan_transitions({sk("a"), sk("b"), sk("c")},
                                         {row("a", now - 100 * day, 0), row("b", now - 40 * day, 0)}, now, cfg);
        REQUIRE(p.transitions.size() == 3);
        CHECK(p.transitions[0].name == "a" && p.transitions[0].action == "archive");
        CHECK(p.transitions[1].name == "b" && p.transitions[1].action == "stale");
        CHECK(p.transitions[2].name == "c" && p.transitions[2].action == "seed");
    }

    TEST_MAIN_END();
}
