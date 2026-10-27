// Background review: the nudge decision table, the three prompts, the autonomous skill guard, and the
// [learning] config table. No model, network or daemon: all files live under a temp dir.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/config.h"
#include "learning/review.h"
#include "skills/skill_tools.h"
#include "skills/skills.h"
#include "test.h"
#include "tools/tools.h"

using namespace judas;
namespace fs = std::filesystem;

static const std::string kEmDash = "\xE2\x80\x94";

static std::string slurp(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

static void put(const fs::path & p, const std::string & text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

static void test_decide() {
    LearningConfig cfg;   // review on, both nudges at 10

    auto d = decide({9, 9, false}, cfg, ReviewTurn::Done);
    CHECK(!d.fire && !d.memory && !d.skills);
    CHECK(d.next.tool_steps == 9 && d.next.turns == 9 && !d.next.in_flight);

    d = decide({10, 0, false}, cfg, ReviewTurn::Done);
    CHECK(d.fire && d.skills && !d.memory);
    CHECK(d.next.tool_steps == 0 && d.next.turns == 0);
    CHECK(d.next.in_flight);

    d = decide({0, 10, false}, cfg, ReviewTurn::Done);
    CHECK(d.fire && d.memory && !d.skills);
    CHECK(d.next.turns == 0 && d.next.tool_steps == 0);

    d = decide({12, 11, false}, cfg, ReviewTurn::Done);
    CHECK(d.fire && d.memory && d.skills);
    CHECK(d.next.tool_steps == 0 && d.next.turns == 0);

    // only the nudge that fired is consumed: the other dimension can still fire later
    d = decide({12, 4, false}, cfg, ReviewTurn::Done);
    CHECK(d.fire && d.skills && !d.memory);
    CHECK(d.next.tool_steps == 0 && d.next.turns == 4);

    // only a completed lead turn may review
    for (ReviewTurn t : {ReviewTurn::Paused, ReviewTurn::Cancelled, ReviewTurn::Failed}) {
        d = decide({99, 99, false}, cfg, t);
        CHECK(!d.fire && !d.next.in_flight);
    }

    // disabled, in flight, and 0 thresholds all suppress
    LearningConfig off = cfg;
    off.review = false;
    CHECK(!decide({99, 99, false}, off, ReviewTurn::Done).fire);
    CHECK(!decide({99, 99, true}, cfg, ReviewTurn::Done).fire);

    LearningConfig zero = cfg;
    zero.review_tool_steps = 0;
    zero.review_memory_turns = 0;
    CHECK(!decide({999, 999, false}, zero, ReviewTurn::Done).fire);
    zero.review_memory_turns = 5;
    CHECK(decide({999, 5, false}, zero, ReviewTurn::Done).memory);
}

static void test_prompts() {
    CHECK(review_prompt(false, false).empty());

    std::string mem = review_prompt(true, false);
    CHECK(mem.find("memory_append") != std::string::npos);
    CHECK(mem.find("Nothing to save") != std::string::npos);
    CHECK(mem.find("skill library") == std::string::npos);
    CHECK(mem.find("Do NOT capture") == std::string::npos);

    std::string skill = review_prompt(false, true);
    CHECK(skill.find("skill_manage") != std::string::npos);
    CHECK(skill.find("Do NOT capture") != std::string::npos);
    CHECK(skill.find("Environment-dependent failures") != std::string::npos);
    CHECK(skill.find("Unresolved failures") != std::string::npos);
    CHECK(skill.find("protected") != std::string::npos);
    CHECK(skill.find("curator adopt") == std::string::npos);
    CHECK(skill.find("/skill-") == std::string::npos);

    std::string both = review_prompt(true, true);
    CHECK(both.find("**Memory**") != std::string::npos);
    CHECK(both.find("**Skills**") != std::string::npos);
    CHECK(both.find("Do NOT capture") != std::string::npos);
    CHECK(both.find("Environment-dependent failures") != std::string::npos);

    for (const std::string * p : {&mem, &skill, &both}) {
        CHECK(p->find(kEmDash) == std::string::npos);
        CHECK(p->find("Hermes") == std::string::npos);
        CHECK(p->find("curator adopt") == std::string::npos);
    }
}

static void test_autonomous_guard() {
    fs::path root = fs::path("/tmp/opencode") / ("judas-review-" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::path dir = root / "skills";
    fs::create_directories(dir);

    put(dir / "user-local.md",
        "---\nname: user-local\ndescription: Hand written\nsource: local\nversion: 1\n---\n\nOriginal body\n");
    put(dir / "pinned-agent.md",
        "---\nname: pinned-agent\ndescription: Pinned agent skill\nsource: agent\npinned: true\nversion: 1\n---\n\n"
        "Pinned body\n");
    put(dir / "agent-owned.md",
        "---\nname: agent-owned\ndescription: Agent skill\nsource: agent\nversion: 1\n---\n\nAgent body\n");

    SkillsConfig sc;
    sc.paths = {dir.string()};
    Skills skills(sc);
    REQUIRE(skills.all().size() == 3);

    Config cfg;
    cfg.skills = sc;
    cfg.permissions.mode = "full-auto";
    cfg.permissions.allow_paths = {root.string()};

    ToolRegistry reg;
    register_skill_tools(reg, skills, nullptr);

    ToolContext ctx;
    ctx.cwd = root.string();
    ctx.cfg = &cfg;
    CHECK(!ctx.autonomous);   // the default is a foreground agent
    ctx.autonomous = true;

    auto call = [&](json args) { return reg.call("skill_manage", args, ctx); };

    // reading is always allowed
    CHECK(call({{"action", "list"}}).ok);
    CHECK(call({{"action", "view"}, {"name", "user-local"}}).ok);

    // every write action refuses a user-owned skill
    auto patch_local = call({{"action", "patch"}, {"name", "user-local"}, {"old_string", "Original"}, {"new_string", "Nope"}});
    CHECK(!patch_local.ok);
    CHECK(patch_local.output.find("protected: user owned") != std::string::npos);
    auto rewrite_local = call({{"action", "rewrite"}, {"name", "user-local"}, {"body", "new"}});
    CHECK(!rewrite_local.ok && rewrite_local.output.find("protected: user owned") != std::string::npos);
    auto archive_local = call({{"action", "archive"}, {"name", "user-local"}});
    CHECK(!archive_local.ok && archive_local.output.find("protected: user owned") != std::string::npos);
    auto pin_local = call({{"action", "pin"}, {"name", "user-local"}});
    CHECK(!pin_local.ok && pin_local.output.find("protected: user owned") != std::string::npos);
    CHECK(slurp(dir / "user-local.md").find("Original body") != std::string::npos);   // nothing was written

    // a pinned skill is protected even when the agent created it
    auto patch_pinned = call({{"action", "patch"}, {"name", "pinned-agent"}, {"old_string", "Pinned"}, {"new_string", "No"}});
    CHECK(!patch_pinned.ok && patch_pinned.output.find("protected: user owned") != std::string::npos);
    auto unpin = call({{"action", "unpin"}, {"name", "pinned-agent"}});
    CHECK(!unpin.ok && unpin.output.find("protected: user owned") != std::string::npos);

    // an agent-owned, unpinned skill is fair game; create is fair game too
    CHECK(call({{"action", "patch"}, {"name", "agent-owned"}, {"old_string", "Agent body"}, {"new_string", "Better body"}}).ok);
    auto created = call({{"action", "create"}, {"name", "learned-rule"}, {"description", "A lesson"}, {"body", "Do it well."}});
    CHECK(created.ok);
    auto learned = skills.by_name("learned-rule");
    REQUIRE(learned.has_value());
    CHECK(learned->source == "agent");

    // a foreground agent can still edit a user-owned skill
    ToolContext fg = ctx;
    fg.autonomous = false;
    CHECK(reg.call("skill_manage",
                   {{"action", "patch"}, {"name", "user-local"}, {"old_string", "Original"}, {"new_string", "Changed"}},
                   fg)
              .ok);
    auto local = skills.by_name("user-local");
    REQUIRE(local.has_value());
    CHECK(local->version == 2);
    CHECK(local->body.find("Changed body") != std::string::npos);

    fs::remove_all(root, ec);
}

static void test_config() {
    Config d;
    CHECK(d.learning.review);
    CHECK(d.learning.review_tool_steps == 10);
    CHECK(d.learning.review_memory_turns == 10);
    CHECK(d.learning.review_max_steps == 12);

    std::string tmpl = default_config_toml();
    CHECK(tmpl.find("[learning]") != std::string::npos);
    CHECK(tmpl.find("review_tool_steps") != std::string::npos);
    CHECK(tmpl.find("review_memory_turns") != std::string::npos);
    CHECK(tmpl.find("review_max_steps") != std::string::npos);
    CHECK(tmpl.find(kEmDash) == std::string::npos);

    fs::path root = fs::path("/tmp/opencode") / ("judas-review-config-" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    fs::path file = root / "config.toml";
    put(file, "[learning]\nreview = false\nreview_tool_steps = 3\nreview_memory_turns = 4\nreview_max_steps = 5\n");

    std::string old;
    bool had = false;
    if (const char * e = std::getenv("JUDAS_CONFIG"); e) {
        old = e;
        had = true;
    }
    setenv("JUDAS_CONFIG", file.c_str(), 1);
    Config c = load_config(root.string());
    if (had) setenv("JUDAS_CONFIG", old.c_str(), 1);
    else unsetenv("JUDAS_CONFIG");

    CHECK(!c.learning.review);
    CHECK(c.learning.review_tool_steps == 3);
    CHECK(c.learning.review_memory_turns == 4);
    CHECK(c.learning.review_max_steps == 5);

    std::string dumped = dump_config(c);
    CHECK(dumped.find("[learning]") != std::string::npos);
    CHECK(dumped.find("review_tool_steps = 3") != std::string::npos);

    fs::remove_all(root, ec);
}

int main() {
    test_decide();
    test_prompts();
    test_autonomous_guard();
    test_config();
    TEST_MAIN_END();
}
