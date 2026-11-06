// Loop-guard detectors driven by scripted model output (no model needed): read loops and alternation, tool calls
// written as text, runs of empty replies, and the per-turn wall-clock budget.
#include <string>
#include <vector>

#include "agent/policy.h"
#include "test.h"

using namespace judas;
using V = LoopGuard::Verdict;

static const std::vector<std::string> kTools = {"shell", "read_file", "grep", "write_file", "done"};

// 1. read loops ------------------------------------------------------------------------------------------------

static void test_read_loops() {
    // The same read repeated: nudge first, pause when it goes on.
    LoopGuard g(3, 2);
    const std::string a = "{\"path\":\"src/a.cpp\"}";
    CHECK(g.observe("read_file", a, true, false, "text").verdict == V::Ok);
    CHECK(g.observe("read_file", a, true, false, "text").verdict == V::Ok);
    auto r = g.observe("read_file", a, true, false, "text");
    CHECK(r.verdict == V::Warn && r.kind == "call" && r.tool == "read_file");
    CHECK(loop_nudge(r, false).find("already have that result") != std::string::npos);
    CHECK(g.observe("read_file", a, true, false, "text").verdict == V::Ok);
    CHECK(g.observe("read_file", a, true, false, "text").verdict == V::Ok);
    r = g.observe("read_file", a, true, false, "text");
    CHECK(r.verdict == V::Pause);
    CHECK(loop_pause_reason(r).find("looping") == 0);

    // grep and shell loops count the same way (different args do not).
    LoopGuard s(3, 2);
    for (int i = 0; i < 2; i++) CHECK(s.observe("grep", "{\"pattern\":\"x\"}", true, false, "m").verdict == V::Ok);
    CHECK(s.observe("grep", "{\"pattern\":\"y\"}", true, false, "m").verdict == V::Ok);
    CHECK(s.observe("grep", "{\"pattern\":\"x\"}", true, false, "m").verdict == V::Warn);
    LoopGuard sh(3, 2);
    for (int i = 0; i < 2; i++) CHECK(sh.observe("shell", "{\"command\":\"ls\"}", true, false, "o").verdict == V::Ok);
    CHECK(sh.observe("shell", "{\"command\":\"ls\"}", true, false, "o").verdict == V::Warn);

    // Alternating between two calls: ONE warning first (it used to jump straight to a pause), a pause if it goes on.
    LoopGuard c(3, 2);
    const std::string x = "{\"path\":\"x\"}", y = "{\"path\":\"y\"}";
    std::vector<V> seen;
    LoopGuard::Result first;
    for (int i = 0; i < 5; i++) {
        auto rr = c.observe("read_file", i % 2 ? y : x, true, false, "t");
        seen.push_back(rr.verdict);
        if (rr.verdict == V::Warn) first = rr;
    }
    CHECK((seen == std::vector<V>{V::Ok, V::Ok, V::Ok, V::Ok, V::Warn}));
    CHECK(first.kind == "cycle");
    CHECK(loop_nudge(first, false).find("taking turns") != std::string::npos);
    CHECK(loop_nudge(first, false).find("already have that result") != std::string::npos);
    // the model reacts and does something else: no pause
    CHECK(c.observe("write_file", "{}", true, true, "ok").verdict == V::Ok);
    CHECK(c.observe("read_file", x, true, false, "t").verdict == V::Ok);
    // the model keeps alternating: the second warning pauses
    V last = V::Ok;
    for (int i = 0; i < 12 && last != V::Pause; i++) last = c.observe("read_file", i % 2 ? y : x, true, false, "t").verdict;
    CHECK(last == V::Pause);
    CHECK(loop_pause_reason(first).find("alternating") != std::string::npos);

    // Alternation across two different tools.
    LoopGuard m(3, 2);
    LoopGuard::Result mr;
    for (int i = 0; i < 5; i++) {
        auto rr = m.observe(i % 2 ? "grep" : "read_file", "same", true, false, "t");
        if (rr.verdict == V::Warn) mr = rr;
    }
    CHECK(mr.kind == "cycle" && mr.tool == "read_file" && mr.other == "grep");
}

// 2. tool call written as text ---------------------------------------------------------------------------------

static void test_text_tool_calls() {
    std::vector<TextCall> out;
    const std::string xml = "<tool_call><function=shell><parameter=command>ls -la\n</parameter></function></tool_call>";
    CHECK(classify_reply(xml, false, "stop") == ReplyKind::TextToolCall);
    CHECK(parse_text_tool_calls(xml, kTools, out) == TextCallParse::Ok);
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "shell" && out[0].args["command"] == "ls -la");

    // text before it, several calls, multi-line values
    const std::string multi = "Let me check.\n<tool_call>\n<function=read_file>\n<parameter=path>\nsrc/a.cpp\n</parameter>\n"
                              "<parameter=max_lines>\n40\n</parameter>\n</function>\n</tool_call>\n"
                              "<tool_call><function=shell><parameter=command>\nline1\nline2\n</parameter></function></tool_call>";
    CHECK(parse_text_tool_calls(multi, kTools, out) == TextCallParse::Ok);
    REQUIRE(out.size() == 2);
    CHECK(out[0].args["path"] == "src/a.cpp" && out[0].args["max_lines"] == "40");
    CHECK(out[1].args["command"] == "line1\nline2");
    // the schema turns "40" into a number
    json schema = {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}, {"max_lines", {{"type", "integer"}}},
                                                       {"all", {{"type", "boolean"}}}}}};
    out[0].args["all"] = "true";
    coerce_text_args(out[0].args, schema);
    CHECK(out[0].args["max_lines"] == 40 && out[0].args["path"] == "src/a.cpp" && out[0].args["all"] == true);

    // JSON variants
    CHECK(parse_text_tool_calls("{\"name\":\"grep\",\"arguments\":{\"pattern\":\"foo\"}}", kTools, out) == TextCallParse::Ok);
    REQUIRE(out.size() == 1);
    CHECK(out[0].name == "grep" && out[0].args["pattern"] == "foo");
    CHECK(parse_text_tool_calls("```json\n{\"name\":\"shell\",\"parameters\":{\"command\":\"pwd\"}}\n```", kTools, out) ==
          TextCallParse::Ok);
    CHECK(out[0].args["command"] == "pwd");
    CHECK(parse_text_tool_calls("<tool_call>{\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"id\\\"}\"}</tool_call>",
                                kTools, out) == TextCallParse::Ok);
    CHECK(out[0].args["command"] == "id");
    CHECK(parse_text_tool_calls("[{\"name\":\"shell\",\"arguments\":{\"command\":\"a\"}},"
                                "{\"name\":\"shell\",\"arguments\":{\"command\":\"b\"}}]", kTools, out) == TextCallParse::Ok);
    CHECK(out.size() == 2);

    // not a call: ordinary answers, prose that quotes JSON, other JSON
    CHECK(parse_text_tool_calls("All done. The tests pass.", kTools, out) == TextCallParse::None && out.empty());
    CHECK(parse_text_tool_calls("Use it like {\"name\":\"shell\",\"arguments\":{\"command\":\"ls\"}} in the config.", kTools,
                                out) == TextCallParse::None);
    CHECK(parse_text_tool_calls("{\"status\":\"ok\",\"count\":3}", kTools, out) == TextCallParse::None);
    CHECK(parse_text_tool_calls("{\"name\":\"widget\"}", kTools, out) == TextCallParse::Malformed);   // unknown tool

    // malformed: cut off, unknown tool, broken parameter
    CHECK(parse_text_tool_calls("<tool_call><function=shell><parameter=command>ls", kTools, out) == TextCallParse::Malformed);
    CHECK(parse_text_tool_calls("<tool_call><function=rm_rf><parameter=path>/</parameter></function></tool_call>", kTools,
                                out) == TextCallParse::Malformed && out.empty());
    CHECK(parse_text_tool_calls("<tool_call><function=shell><parameter=command>ls</function></tool_call>", kTools, out) ==
          TextCallParse::Malformed);
    CHECK(parse_text_tool_calls("{\"name\":\"shell\",\"arguments\":{\"command\":\"ls\"", kTools, out) == TextCallParse::Malformed);
    CHECK(parse_text_tool_calls("```json\n{\"name\": \"shell\", \"arguments\": {oops}}\n```", kTools, out) ==
          TextCallParse::Malformed);
    CHECK(parse_text_tool_calls("{not json, \"name\" only}", kTools, out) == TextCallParse::None);
    CHECK(parse_text_tool_calls("<tool_call>not json</tool_call>", kTools, out) == TextCallParse::Malformed);

    // Malformed text asks for a retry through the normal continuation path, and it is capped.
    TurnState st;
    PlanState none;
    auto d = should_continue(ReplyKind::TextToolCall, none, st, 2, 10);
    CHECK(d.action == Action::Continue && d.nudge.find("real tool call") != std::string::npos);
    should_continue(ReplyKind::TextToolCall, none, st, 2, 10);
    CHECK(should_continue(ReplyKind::TextToolCall, none, st, 2, 10).action == Action::Pause);
}

// 3. empty turns -> wrap-up ------------------------------------------------------------------------------------

static void test_empty_wrap_up() {
    PlanState open2;
    open2.open = 2;
    TurnState st;
    auto d = should_continue(ReplyKind::Empty, open2, st, 8, 5);
    CHECK(d.action == Action::Continue && !st.wrapping && d.nudge.find("Stop working") == std::string::npos);
    d = should_continue(ReplyKind::Empty, open2, st, 8, 5);   // second empty reply in a row
    CHECK(d.action == Action::Continue && st.wrapping);
    CHECK(d.nudge.find("summarize what is done and what is left") != std::string::npos);
    // the summary ends the turn even though plan items are open
    CHECK(should_continue(ReplyKind::Final, open2, st, 8, 5).action == Action::Finish);

    // still empty after the instruction: pause, with a reason
    TurnState s2;
    should_continue(ReplyKind::Empty, open2, s2, 8, 5);
    should_continue(ReplyKind::Empty, open2, s2, 8, 5);
    d = should_continue(ReplyKind::Empty, open2, s2, 8, 5);
    CHECK(d.action == Action::Pause && d.reason.find("wrap-up") != std::string::npos);

    // empty replies that are not consecutive never trigger it
    TurnState s3;
    for (int i = 0; i < 4; i++) {
        should_continue(ReplyKind::Empty, open2, s3, 8, 5);
        should_continue(ReplyKind::ToolCalls, open2, s3, 8, 5);
    }
    CHECK(!s3.wrapping);
}

// 4. wall-clock budget ------------------------------------------------------------------------------------------

static void test_wall_budget() {
    CHECK(!wall_budget_exceeded(1e9, 0));          // off
    CHECK(!wall_budget_exceeded(59.9, 60));
    CHECK(wall_budget_exceeded(60.0, 60));
    CHECK(wall_budget_exceeded(61.0, 60));
    std::string m = wrap_up_message("The time budget for this turn (60 s) is used up.");
    CHECK(m.find("time budget") != std::string::npos && m.find("without a tool call") != std::string::npos);
    CHECK(m.find("summarize what is done and what is left") != std::string::npos);
}

int main() {
    test_read_loops();
    test_text_tool_calls();
    test_empty_wrap_up();
    test_wall_budget();
    TEST_MAIN_END();
}
