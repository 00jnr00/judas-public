// Offline checks for the agent turn policy (reply classification, continuation, loop guard, delegation nudge)
// and for the per-edit git step (dedicated ref, user's branch and index untouched, nothing swept in).
#include "agent/gitstep.h"
#include "agent/policy.h"
#include "core/config.h"
#include "tools/tools.h"
#include "test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace judas;
namespace fs = std::filesystem;

static void test_classify() {
    CHECK(classify_reply("", false, "stop") == ReplyKind::Empty);
    CHECK(classify_reply("  \n", false, "stop") == ReplyKind::Empty);
    CHECK(classify_reply("Let me view the file:", false, "stop") == ReplyKind::Announce);
    CHECK(classify_reply("<tool_call>\n<function=shell>", false, "stop") == ReplyKind::TextToolCall);
    CHECK(classify_reply("Done. I fixed the bug and the tests pass.", false, "stop") == ReplyKind::Final);
    // no keyword gating: a real answer that happens to say "let me know" is final
    CHECK(classify_reply("All set. Let me know if you want anything else.", false, "stop") == ReplyKind::Final);
    CHECK(classify_reply("", true, "tool_calls") == ReplyKind::ToolCalls);
    CHECK(classify_reply("x", true, "length") == ReplyKind::Truncated);  // a cut off call never runs
    CHECK(classify_reply("", false, "length") == ReplyKind::Truncated);
    std::string longc(500, 'a');
    longc += ":";
    CHECK(classify_reply(longc, false, "stop") == ReplyKind::Final);  // long text ending in a colon is an answer
}

static void test_announce() {
    // Replies that only say what the agent is about to do: no tool call means the turn must continue.
    for (const char * s : {
             "Let me read the file.",
             "I'll fix the regex.",
             "Now I will run the tests.",
             "Next, I need to check the build log.",
             "Let's look at the failing test.",
             "I am going to update the header.",
             "I'm about to run ctest.",
             "Done with the read. Now let me edit the file.",
             "Then I'll rerun the suite.",
             "First, I will reproduce the failure.",
             "- Let me inspect the diff.",
             "**I'll check the config.**",
             "I need to read the header before editing.",
             "Let us run the tests.",
             "I\u2019ll fix the parser.",
         })
        CHECK(classify_reply(s, false, "stop") == ReplyKind::Announce);

    // Real answers, questions and sign-offs end the turn even when they mention what the agent could do.
    for (const char * s : {
             "Done. I fixed the bug and the tests pass.",
             "All set. Let me know if you want anything else.",
             "The fix is in. Let me know if you want changes.",
             "Should I run the tests?",
             "Would you like me to continue?",
             "I'll summarize: the build failed on a missing include. The fix is in.",
             "I fixed the regex.",
             "I've updated the tests and they pass.",
             "The tests pass now.",
             "I can run the tests if you want.",
             "I'd rather keep the current API.",
             "If you want, I'll run the tests.",
             "No further action is needed.",
             "Let me run the tests?",
             "I will not change the public API.",
             "Let me tell you what changed. The parser is simpler now.",
             "The parser now handles empty input. I'll note that the fix touches only policy.cpp. The rest of "
             "the pipeline is unchanged.",
         })
        CHECK(classify_reply(s, false, "stop") == ReplyKind::Final);

    // A parsed tool call wins, a cut off reply never runs, and only a clean stop can announce.
    CHECK(classify_reply("I'll fix it.", true, "stop") == ReplyKind::ToolCalls);
    CHECK(classify_reply("I'll fix it.", false, "length") == ReplyKind::Truncated);
    CHECK(classify_reply("I'll fix it.", false, "") == ReplyKind::Final);
    CHECK(classify_reply("I'll fix it.", false, "cancelled") == ReplyKind::Final);

    // The continuation nudge names the missing call, and the cap still applies.
    TurnState st;
    PlanState none;
    auto d = should_continue(ReplyKind::Announce, none, st, 3, 10);
    CHECK(d.action == Action::Continue && st.auto_continues == 1);
    CHECK(d.nudge.find("You said what you would do next but made no tool call. Make that call now.") != std::string::npos);
    should_continue(ReplyKind::Announce, none, st, 3, 10);
    d = should_continue(ReplyKind::Announce, none, st, 3, 10);
    CHECK(d.action == Action::Continue && st.auto_continues == 3);
    d = should_continue(ReplyKind::Announce, none, st, 3, 10);
    CHECK(d.action == Action::Pause && d.reason.find("announce") != std::string::npos);
}

static void test_plan() {
    CHECK(plan_state("").open == 0);
    auto p = plan_state("- [x] one\n- [ ] two\n* [ ] three\n1. [ ] four\n2) [X] five\n  - [ ] six\nnotes\n");
    CHECK(p.open == 4 && p.done == 2);
    CHECK(plan_state("- [x] a\n- [x] b").open == 0);
    CHECK(plan_state("just text, no boxes").open == 0);
}

static void test_continue() {
    TurnState st;
    PlanState none, open2;
    open2.open = 2;
    auto d = should_continue(ReplyKind::ToolCalls, open2, st, 3, 10);
    CHECK(d.action == Action::Execute && st.auto_continues == 0);
    d = should_continue(ReplyKind::Final, none, st, 3, 10);
    CHECK(d.action == Action::Finish);
    // a final looking reply with open plan items is not final
    d = should_continue(ReplyKind::Final, open2, st, 3, 10);
    CHECK(d.action == Action::Continue && d.nudge.find("2 open") != std::string::npos && st.auto_continues == 1);
    d = should_continue(ReplyKind::Empty, none, st, 3, 10);
    CHECK(d.action == Action::Continue && d.nudge.find("call done") != std::string::npos && st.auto_continues == 2);
    d = should_continue(ReplyKind::Announce, none, st, 3, 10);
    CHECK(d.action == Action::Continue && st.auto_continues == 3);
    // cap reached: pause, never done
    d = should_continue(ReplyKind::Empty, open2, st, 3, 10);
    CHECK(d.action == Action::Pause && !d.reason.empty());
    d = should_continue(ReplyKind::Final, open2, st, 3, 10);
    CHECK(d.action == Action::Pause);
    // a final answer with no open items still finishes even at the cap
    d = should_continue(ReplyKind::Final, none, st, 3, 10);
    CHECK(d.action == Action::Finish);

    TurnState t2;
    d = should_continue(ReplyKind::Truncated, none, t2, 2, 8192);
    CHECK(d.action == Action::Continue && d.nudge.find("8192") != std::string::npos && t2.truncations == 1);
    CHECK(truncation_nudge(true, 8192).find("append=true") != std::string::npos);
    CHECK(truncation_nudge(true, 8192).find("NOT run") != std::string::npos);
    // zero cap pauses at once
    TurnState t3;
    CHECK(should_continue(ReplyKind::Empty, none, t3, 0, 1).action == Action::Pause);
}

static void test_loop() {
    LoopGuard g(3, 2);
    using V = LoopGuard::Verdict;
    CHECK(g.observe("shell", "{\"command\":\"ls\"}", true, false, "a").verdict == V::Ok);
    CHECK(g.observe("shell", "{\"command\":\"ls\"}", true, false, "a").verdict == V::Ok);
    auto r = g.observe("shell", "{\"command\":\"ls\"}", true, false, "a");
    CHECK(r.verdict == V::Warn && r.kind == "call" && r.count == 3);
    CHECK(loop_nudge(r, true).find("looping") != std::string::npos);
    CHECK(loop_nudge(r, true).find("spawn a worker") != std::string::npos);
    CHECK(loop_nudge(r, false).find("spawn a worker") == std::string::npos);
    // ignoring the warning: three more identical calls pause the turn
    g.observe("shell", "{\"command\":\"ls\"}", true, false, "a");
    g.observe("shell", "{\"command\":\"ls\"}", true, false, "a");
    CHECK(g.observe("shell", "{\"command\":\"ls\"}", true, false, "a").verdict == V::Pause);

    // same error with different args and shifting line numbers still counts
    LoopGuard e(3, 2);
    CHECK(e.observe("shell", "cargo build 1", false, false, "error[E0382] at src/a.rs:10:5 took 1.2s").verdict == V::Ok);
    CHECK(e.observe("edit_file", "x", true, true, "edited").verdict == V::Ok);
    CHECK(e.observe("shell", "cargo build 2", false, false, "error[E0382]  at src/a.rs:12:5 took 2.9s").verdict == V::Ok);
    r = e.observe("shell", "cargo build 3", false, false, "error[E0382] at src/a.rs:14:5 took 3.1s");
    CHECK(r.verdict == V::Warn && r.kind == "error");

    // a successful write resets the call evidence (re-reading after an edit is fine)
    LoopGuard w(3, 2);
    w.observe("read_file", "a", true, false, "x");
    w.observe("read_file", "a", true, false, "x");
    w.observe("edit_file", "a", true, true, "ok");
    CHECK(w.observe("read_file", "a", true, false, "x").verdict == V::Ok);

    // tools exempt from tracking never trip it
    LoopGuard x(3, 2);
    for (int i = 0; i < 6; i++) CHECK(x.observe("update_plan", "p", true, false, "ok", false).verdict == V::Ok);

    // different calls do not trip it
    LoopGuard y(3, 2);
    for (int i = 0; i < 20; i++)
        CHECK(y.observe("read_file", "f" + std::to_string(i), true, false, "x").verdict == V::Ok);

    CHECK(normalize_error("a  1\n2 b") == "a b");
}

static void test_read_streak() {
    CHECK(classify_call("read_file", "", false) == CallKind::Read);
    CHECK(classify_call("grep", "", false) == CallKind::Read);
    CHECK(classify_call("recall", "", false) == CallKind::Read);
    CHECK(classify_call("edit_file", "", true) == CallKind::Reset);
    CHECK(classify_call("write_file", "", true) == CallKind::Reset);
    for (const char * t : {"spawn_agent", "wait_agents", "send_agent", "update_plan", "done", "notify"})
        CHECK(classify_call(t, "", false) == CallKind::Neutral);
    for (const char * c : {"cat a.txt", "  head -n 5 x", "sed -n 1,20p f", "ls -la", "find . -name '*.cpp'", "grep -rn x .",
                           "rg foo", "wc -l f", "tree src", "git log --oneline", "git diff", "git status", "git show HEAD",
                           "cargo tree", "cargo metadata", "cd src && cat a.h | head", "grep x f 2>&1", "ls > /dev/null"})
        CHECK(classify_call("shell", c, false) == CallKind::Read);
    for (const char * c : {"cargo build", "cargo test", "cargo check", "make -j4", "cmake --build build", "ctest",
                           "pytest -q", "npm test", "sed -i s/a/b/ f", "echo hi > f", "cat a >> b", "git commit -m x",
                           "ls && make", "find . -delete", "python3 x.py", "rm f"})
        CHECK(classify_call("shell", c, false) == CallKind::Reset);
    CHECK(classify_call("shell", "cd x", false) == CallKind::Neutral);

    ReadStreak st;
    int nudges = 0, forces = 0;
    for (int i = 1; i <= 20; i++) {
        auto ev = read_streak_observe(st, CallKind::Read, 8, 14);
        if (ev == StreakEvent::Nudge) { nudges++; CHECK(i == 8); }
        if (ev == StreakEvent::Force) { forces++; CHECK(i == 14); }
        CHECK(read_streak_blocked(st, 14) == (i >= 14));
    }
    CHECK(nudges == 1 && forces == 1);
    CHECK(read_streak_observe(st, CallKind::Neutral, 8, 14) == StreakEvent::None);
    CHECK(read_streak_blocked(st, 14));            // neutral calls do not clear a block
    read_streak_observe(st, CallKind::Reset, 8, 14);
    CHECK(st.count == 0 && !read_streak_blocked(st, 14));
    // 0 disables
    ReadStreak off;
    for (int i = 0; i < 30; i++) CHECK(read_streak_observe(off, CallKind::Read, 0, 0) == StreakEvent::None);
    CHECK(!read_streak_blocked(off, 0));
    CHECK(read_streak_message(StreakEvent::Nudge, 8).find("8 read-only calls") != std::string::npos);
    CHECK(read_streak_message(StreakEvent::Force, 14).find("blocked") != std::string::npos);
    CHECK(read_streak_message(StreakEvent::Force, 14).find("final answer") != std::string::npos);   // reviews may answer

    // cap_tool_result: under the cap unchanged; over it keeps head and tail and names the recall id
    CHECK(cap_tool_result("short", 100, "c1") == "short");
    CHECK(cap_tool_result(std::string(500, 'x'), 0, "c1").size() == 500);
    std::string big = "HEAD" + std::string(100000, 'm') + "TAIL";
    std::string capped = cap_tool_result(big, 3000, "call_9");
    CHECK(capped.size() < 3300 && capped.rfind("HEAD", 0) == 0 && capped.find("TAIL") == capped.size() - 4);
    CHECK(capped.find("recall id=call_9") != std::string::npos);
    std::string utf = std::string(2999, 'a') + "\xC3\xA9" + std::string(5000, 'b');   // a 2-byte char at the cut
    std::string cu = cap_tool_result(utf, 4000, "");
    CHECK(cu.find("recall id") == std::string::npos);
}

static void test_delegation() {
    DelegationState st;
    // fewer than threshold edits: no nudge
    for (int i = 0; i < 3; i++) CHECK(!delegation_nudge(st, true, false, 3, 4, true));
    CHECK(delegation_nudge(st, true, false, 3, 4, true));   // 4th edit
    CHECK(st.consecutive_edits == 0);                       // restarts
    // not enough open items: never nudges
    DelegationState a;
    for (int i = 0; i < 10; i++) CHECK(!delegation_nudge(a, true, false, 1, 4, true));
    // cannot spawn (workers): never nudges
    DelegationState b;
    for (int i = 0; i < 10; i++) CHECK(!delegation_nudge(b, true, false, 5, 4, false));
    // disabled
    DelegationState c;
    for (int i = 0; i < 10; i++) CHECK(!delegation_nudge(c, true, false, 5, 0, true));
    // a spawn resets the streak; reads and shells do not count
    DelegationState d;
    for (int i = 0; i < 3; i++) delegation_nudge(d, true, false, 3, 4, true);
    delegation_nudge(d, false, true, 3, 4, true);
    CHECK(d.consecutive_edits == 0);
    for (int i = 0; i < 3; i++) CHECK(!delegation_nudge(d, true, false, 3, 4, true));
    CHECK(!delegation_nudge(d, false, false, 3, 4, true));
}

static int sh(const std::string & c) { return std::system(c.c_str()); }

static std::string slurp_cmd(const std::string & c) {
    std::string out;
    FILE * f = popen(c.c_str(), "r");
    char buf[512];
    while (f && fgets(buf, sizeof buf, f)) out += buf;
    if (f) pclose(f);
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

static void test_gitstep() {
    fs::path d = JUDAS_TEST_TMP "/judas-gitstep-test";
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d / "src");
    fs::create_directories(d / "target");
    std::string g = "git -C '" + d.string() + "' ";
    REQUIRE(sh(g + "init -q -b main && " + g + "config user.email t@t && " + g + "config user.name t") == 0);
    std::ofstream(d / ".gitignore") << "*.log\n";
    std::ofstream(d / "src/a.rs") << "one\n";
    REQUIRE(sh(g + "add -A && " + g + "commit -q -m init") == 0);
    std::string branch0 = slurp_cmd(g + "rev-parse --abbrev-ref HEAD");
    std::string head0 = slurp_cmd(g + "rev-parse HEAD");

    // user has an unrelated dirty file and staged change; build junk exists
    std::ofstream(d / "notes.txt") << "mine\n";
    REQUIRE(sh(g + "add notes.txt") == 0);
    std::ofstream(d / "target/junk.o") << "junk\n";
    std::ofstream(d / "debug.log") << "ignored\n";
    std::ofstream(d / "src/a.rs") << "two\n";
    std::string status0 = slurp_cmd(g + "status --porcelain");

    auto r = git_commit_step(d.string(), "judas/", "sess1234", "judas: edit src/a.rs", {(d / "src/a.rs").string()});
    CHECK(r.committed);
    CHECK(r.ref == "refs/judas/sess1234");
    CHECK(slurp_cmd(g + "rev-parse --abbrev-ref HEAD") == branch0);            // branch not switched
    CHECK(slurp_cmd(g + "rev-parse HEAD") == head0);                           // user's branch not advanced
    CHECK(slurp_cmd(g + "status --porcelain") == status0);                     // real index and tree untouched
    CHECK(slurp_cmd(g + "show --stat --format= refs/judas/sess1234").find("src/a.rs") != std::string::npos);
    CHECK(slurp_cmd(g + "show --stat --format= refs/judas/sess1234").find("notes.txt") == std::string::npos);
    CHECK(slurp_cmd(g + "rev-parse refs/judas/sess1234~1") == head0);          // branches off HEAD
    CHECK(!fs::exists(d / ".git" / "judas-index-sess1234"));

    // no change: no new commit
    auto r2 = git_commit_step(d.string(), "judas/", "sess1234", "again", {(d / "src/a.rs").string()});
    CHECK(!r2.committed && r2.error.empty());

    // fallback with no paths: ignored files and build dirs stay out
    std::ofstream(d / "src/b.rs") << "new\n";
    auto r3 = git_commit_step(d.string(), "judas/", "sess1234", "all", {});
    CHECK(r3.committed);
    std::string files = slurp_cmd(g + "ls-tree -r --name-only refs/judas/sess1234");
    CHECK(files.find("src/b.rs") != std::string::npos);
    CHECK(files.find("target") == std::string::npos);
    CHECK(files.find("debug.log") == std::string::npos);
    CHECK(slurp_cmd(g + "rev-list --count refs/judas/sess1234") == "3");

    // parallel workers on the same repo and ref: all commits land, no lock errors
    std::vector<std::thread> ts;
    std::vector<int> ok(6, 0);
    for (int i = 0; i < 6; i++)
        ts.emplace_back([&, i] {
            std::string f = "src/p" + std::to_string(i) + ".rs";
            std::ofstream(d / f) << "p" << i << "\n";
            auto rr = git_commit_step(d.string(), "judas/", "sess1234", "par " + f, {(d / f).string()});
            ok[i] = rr.committed && rr.error.empty();
        });
    for (auto & t : ts) t.join();
    int n = 0;
    for (int v : ok) n += v;
    CHECK(n == 6);
    CHECK(slurp_cmd(g + "rev-list --count refs/judas/sess1234") == "9");

    // not a repo: reported, harmless
    fs::create_directories(JUDAS_TEST_TMP "/judas-gitstep-norepo");
    auto r4 = git_commit_step(JUDAS_TEST_TMP "/judas-gitstep-norepo", "judas/", "s", "m", {});
    CHECK(!r4.committed && !r4.error.empty());
    fs::remove_all(d, ec);
}

static void test_append() {
    Config cfg;
    cfg.permissions.mode = "full-auto";
    cfg.permissions.allow_paths = {JUDAS_TEST_TMP};
    ToolRegistry reg;
    register_builtin_tools(reg);
    fs::create_directories(JUDAS_TEST_TMP "/judas-append-test");
    std::string f = JUDAS_TEST_TMP "/judas-append-test/big.txt";
    ToolContext ctx;
    ctx.cwd = JUDAS_TEST_TMP;
    ctx.cfg = &cfg;
    CHECK(reg.call("write_file", {{"path", f}, {"content", "part1\n"}}, ctx).ok);
    CHECK(reg.call("write_file", {{"path", f}, {"content", "part2\n"}, {"append", true}}, ctx).ok);
    std::ifstream in(f);
    std::string all((std::istreambuf_iterator<char>(in)), {});
    CHECK(all == "part1\npart2\n");
    CHECK(reg.call("write_file", {{"path", f}, {"content", "x"}}, ctx).ok);   // default still overwrites
    std::ifstream in2(f);
    std::string all2((std::istreambuf_iterator<char>(in2)), {});
    CHECK(all2 == "x");
}

int main() {
    test_append();
    test_classify();
    test_announce();
    test_plan();
    test_continue();
    test_loop();
    test_delegation();
    test_read_streak();
    test_gitstep();
    TEST_MAIN_END();
}
