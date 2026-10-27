// TUI view: event-to-render mapping (Stream), history rendering, and the input box / menu / status layout.
#include "core/json.h"
#include "test.h"
#include "tui/layout.h"
#include "tui/view.h"

using namespace judas::tui;
using judas::json;

static std::string joined(const std::vector<std::string> & rows) {
    std::string o;
    for (auto & r : rows) o += strip_ansi(r) + "\n";
    return o;
}

static json ev(const char * type, json data, const char * agent = "L1", const char * parent = "", const char * role = "lead") {
    return {{"type", type}, {"session", "S"}, {"agent", agent}, {"parent", parent}, {"role", role}, {"data", data}};
}

int main() {
    Palette pal = Palette::plain();
    {   // streaming text commits whole lines, keeps the partial one live
        Stream s(pal, 60);
        s.on_event(ev("spawned", {{"task", "hi"}, {"role", "lead"}}));
        CHECK(s.running());
        s.on_event(ev("delta", {{"text", "Hello wor"}}));
        CHECK(s.take_committed().empty());
        CHECK(joined(s.live(0, 1.2)).find("\xE2\x97\x8F Hello wor") != std::string::npos);
        s.on_event(ev("delta", {{"text", "ld\nsecond line\npartial"}}));
        auto c = joined(s.take_committed());
        CHECK(c == "\xE2\x97\x8F Hello world\n  second line\n");
        s.on_event(ev("done", {{"result", "ok"}, {"status", "done"}}));
        c = joined(s.take_committed());
        CHECK(c == "  partial\n");
        CHECK(!s.running() && s.turn_status() == "done");
        auto r = s.replies();
        REQUIRE(r.size() == 1);
        CHECK(r[0] == "Hello world\nsecond line\npartial");
    }
    {   // a tool call is a live item until its result, then two committed rows
        Stream s(pal, 80);
        s.on_event(ev("spawned", {{"task", "x"}, {"role", "lead"}}));
        s.on_event(ev("tool_call", {{"id", "c1"}, {"name", "read_file"}, {"args", {{"path", "src/x.cpp"}}}}));
        CHECK(s.take_committed().empty());
        CHECK(joined(s.live(0, 0)).find("read_file src/x.cpp") != std::string::npos);
        s.on_event(ev("tool_result", {{"id", "c1"}, {"name", "read_file"}, {"ok", true}, {"output", "line1\nline2\nline3\n"}}));
        auto c = joined(s.take_committed());
        CHECK(c == "\xE2\x97\x8F read_file src/x.cpp\n  \xE2\x8E\xBF line1 (+2 lines) /expand 1\n");
        CHECK(s.tool(0) && s.tool(0)->output == "line1\nline2\nline3\n");
        CHECK(joined(s.expand_rows(1)).find("line3") != std::string::npos);
        // failure
        s.on_event(ev("tool_call", {{"id", "c2"}, {"name", "shell"}, {"args", {{"command", "false"}}}}));
        s.on_event(ev("tool_result", {{"id", "c2"}, {"name", "shell"}, {"ok", false}, {"output", "exit 1"}}));
        CHECK(joined(s.take_committed()) == "\xE2\x97\x8F shell false\n  \xE2\x8E\xBF exit 1\n");
    }
    {   // results arriving out of order still commit in call order
        Stream s(pal, 80);
        s.on_event(ev("spawned", {{"task", "x"}, {"role", "lead"}}));
        s.on_event(ev("tool_call", {{"id", "a"}, {"name", "t1"}, {"args", "{}"}}));
        s.on_event(ev("tool_call", {{"id", "b"}, {"name", "t2"}, {"args", "{}"}}));
        s.on_event(ev("tool_result", {{"id", "b"}, {"name", "t2"}, {"ok", true}, {"output", "B"}}));
        CHECK(s.take_committed().empty());
        s.on_event(ev("tool_result", {{"id", "a"}, {"name", "t1"}, {"ok", true}, {"output", "A"}}));
        auto c = joined(s.take_committed());
        CHECK(c.find("t1") < c.find("t2"));
    }
    {   // thinking: collapsed by default, full in verbose
        Stream s(pal, 60);
        s.on_event(ev("reasoning", {{"text", "step one\nstep two"}}));
        CHECK(joined(s.live(0, 0)).find("Thinking... step two") != std::string::npos);
        s.on_event(ev("delta", {{"text", "answer\n"}}));
        auto c = joined(s.take_committed());
        CHECK(c.find("Thinking (2 lines, ctrl+o to show)") != std::string::npos);
        CHECK(c.find("step one") == std::string::npos);
        Stream v(pal, 60);
        v.set_verbose(true);
        v.on_event(ev("reasoning", {{"text", "step one\nstep two"}}));
        v.on_event(ev("delta", {{"text", "x\n"}}));
        c = joined(v.take_committed());
        CHECK(c.find("step one") != std::string::npos && c.find("step two") != std::string::npos);
    }
    {   // workers: indented sub-items with status
        Stream s(pal, 80);
        s.on_event(ev("spawned", {{"task", "big job"}, {"role", "lead"}}));
        s.on_event(ev("spawned", {{"task", "scan the repo"}, {"role", "scout"}}, "W1", "L1", "scout"));
        auto c = joined(s.take_committed());
        CHECK(c == "  \xE2\x97\x87 scout  scan the repo\n");
        CHECK(joined(s.live(1, 0)).find("scout [running]") != std::string::npos);
        s.on_event(ev("tool_call", {{"id", "w"}, {"name", "grep"}, {"args", {{"pattern", "foo"}}}}, "W1", "L1", "scout"));
        s.on_event(ev("tool_result", {{"id", "w"}, {"name", "grep"}, {"ok", true}, {"output", "hit"}}, "W1", "L1", "scout"));
        CHECK(joined(s.take_committed()) == "  \xE2\x97\x8F grep foo\n    \xE2\x8E\xBF hit\n");
        s.on_event(ev("done", {{"result", "found it"}, {"status", "done"}}, "W1", "L1", "scout"));
        CHECK(joined(s.take_committed()) == "  \xE2\x9C\x93 scout: found it\n");
        CHECK(s.running());                                              // a worker finishing does not end the turn
        CHECK(joined(s.live(0, 0)).find("scout [") == std::string::npos);
    }
    {   // paused turn, plan tracking, errors, approval request
        Stream s(pal, 80);
        s.on_event(ev("spawned", {{"task", "x"}, {"role", "lead"}}));
        s.on_event(ev("tool_call", {{"id", "p"}, {"name", "update_plan"}, {"args", {{"plan", "- [ ] one\n- [x] two"}}}}));
        s.on_event(ev("tool_result", {{"id", "p"}, {"name", "update_plan"}, {"ok", true}, {"output", "plan updated"}}));
        CHECK(s.plan() == "- [ ] one\n- [x] two");
        s.take_committed();
        s.on_event(ev("status", {{"error", "engine exploded"}}));
        CHECK(joined(s.take_committed()) == "engine exploded\n");
        s.on_event(ev("approval_request", {{"id", "r1"}, {"what", "run: rm -rf x"}}));
        REQUIRE(s.approval.has_value());
        CHECK(s.approval->what == "run: rm -rf x");
        s.on_event(ev("done", {{"result", "[paused] cap"}, {"status", "paused"}, {"reason", "step limit"}}));
        CHECK(s.turn_status() == "paused" && s.pause_reason() == "step limit");
        CHECK(!s.approval.has_value());
        CHECK(joined(s.take_committed()).find("paused: step limit  (/resume to continue)") != std::string::npos);
    }
    {   // history from stored messages
        json agents = json::array({{{"id", "L1"}, {"parent", ""}, {"role", "lead"}}});
        json msgs = json::array({
            {{"id", 1}, {"agent", "L1"}, {"role", "user"}, {"body", {{"role", "user"}, {"content", "list files"}}}},
            {{"id", 2}, {"agent", "L1"}, {"role", "assistant"},
             {"body", {{"role", "assistant"}, {"content", "Looking."}, {"tool_calls", json::array({{{"id", "t1"}, {"type", "function"},
                       {"function", {{"name", "shell"}, {"arguments", "{\"command\":\"ls\"}"}}}}})}}}},
            {{"id", 3}, {"agent", "L1"}, {"role", "tool"}, {"body", {{"role", "tool"}, {"tool_call_id", "t1"}, {"content", "a\nb"}}}},
            {{"id", 4}, {"agent", "L1"}, {"role", "assistant"}, {"body", {{"role", "assistant"}, {"content", "Two files."}}}},
            {{"id", 5}, {"agent", "W9"}, {"role", "assistant"}, {"body", {{"role", "assistant"}, {"content", "worker chatter"}}}},
        });
        Stream s(pal, 60);
        s.load_history(msgs, agents);
        auto c = joined(s.take_committed());
        CHECK(c.find("> list files") != std::string::npos);
        CHECK(c.find("shell ls") != std::string::npos);
        CHECK(c.find("\xE2\x8E\xBF a (+1 lines)") != std::string::npos);
        CHECK(c.find("Two files.") != std::string::npos);
        CHECK(c.find("worker chatter") == std::string::npos);
        auto r = s.replies();
        REQUIRE(r.size() == 1);
        CHECK(r[0] == "Looking.Two files.");
        Stream part(pal, 60);
        part.load_history(msgs, agents, 3);                              // msg_floor: later messages come from replay
        CHECK(joined(part.take_committed()).find("Two files.") == std::string::npos);
    }
    {   // user message and /copy bookkeeping across turns
        Stream s(pal, 40);
        s.user_message("first");
        s.on_event(ev("spawned", {{"task", "first"}, {"role", "lead"}}));
        s.on_event(ev("delta", {{"text", "reply one"}}));
        s.on_event(ev("done", {{"result", ""}, {"status", "done"}}));
        s.user_message("second\nline");
        s.on_event(ev("spawned", {{"task", "second"}, {"role", "lead"}}));
        s.on_event(ev("delta", {{"text", "reply two"}}));
        auto r = s.replies();
        REQUIRE(r.size() == 2);
        CHECK(r[0] == "reply one" && r[1] == "reply two");               // in-progress reply counts
        auto c = joined(s.take_committed());
        CHECK(c.find("> second\n  line") != std::string::npos);
    }
    {   // tool_label
        CHECK(tool_label("read_file", json{{"path", "a/b.txt"}}) == "a/b.txt");
        CHECK(tool_label("shell", "{\"command\":\"ls -la\"}") == "ls -la");
        CHECK(tool_label("x", json{{"foo", 1}}) == "{\"foo\":1}");
        CHECK(tool_label("update_plan", json{{"plan", "x"}}) == "");
        CHECK(output_summary("", 40) == "(no output)");
    }
    {   // input box geometry
        Editor e;
        auto b = render_input_box(e, 40, pal, "Message", false);
        REQUIRE(b.rows.size() == 3);
        for (auto & r : b.rows) CHECK(display_width(r) == 40);
        CHECK(b.cursor_row == 1 && b.cursor_col == 4);
        CHECK(strip_ansi(b.rows[1]).find("> Message") != std::string::npos);
        e.insert("hello");
        b = render_input_box(e, 40, pal, "Message", false);
        CHECK(b.cursor_col == 4 + 5);
        e.insert("\nsecond");
        b = render_input_box(e, 40, pal, "Message", false);
        REQUIRE(b.rows.size() == 4);
        CHECK(b.cursor_row == 2 && b.cursor_col == 4 + 6);
        e.set(std::string(100, 'x'));                                    // wraps to several rows, cursor on the last
        b = render_input_box(e, 30, pal, "", false);
        CHECK(b.rows.size() >= 5);
        CHECK(b.cursor_row == (int) b.rows.size() - 2);
        Editor many;
        for (int i = 0; i < 30; i++) many.insert("l" + std::to_string(i) + "\n");
        b = render_input_box(many, 40, pal, "", false, 6);
        CHECK(b.rows.size() == 8);                                       // capped, cursor kept in view
        CHECK(b.cursor_row == 7 - 1);
        Editor sh;
        sh.insert("!ls");
        b = render_input_box(sh, 40, Palette::from_skin("judas"), "", true);
        CHECK(strip_ansi(b.rows[1]).find("! !ls") != std::string::npos);
    }
    {   // menu and status
        MenuContext ctx;
        auto m = compute_menu("/", 1, ctx);
        m.sel = 2;
        auto rows = render_menu(m, 60, pal, 5);
        REQUIRE(rows.size() == 5);
        int arrows = 0;
        for (auto & r : rows) if (r.rfind("  > ", 0) == 0) arrows++;
        CHECK(arrows == 1);
        CHECK(rows.back().find("(3/") != std::string::npos);
        StatusInfo si;
        si.model = "big"; si.mode = "code"; si.session = "abcdef1234563456"; si.turn = "running";
        si.kv_used = 12345; si.kv_total = 131072; si.queued = 2; si.tok_s = 42.5; si.pending_msgs = 1;
        std::string line = strip_ansi(render_status(si, 100, pal));
        CHECK(line.find("big") != std::string::npos && line.find("code") != std::string::npos);
        CHECK(line.find("KV 12.3k/131k") != std::string::npos && line.find("q 2") != std::string::npos);
        CHECK(line.find("42.5 tok/s") != std::string::npos && line.find("abcdef123456") != std::string::npos);
        CHECK(line.find("running (+1 queued)") != std::string::npos);
        CHECK(display_width(line) <= 100);
        std::string narrow = strip_ansi(render_status(si, 40, pal));
        CHECK(display_width(narrow) <= 40);
        CHECK(narrow.find("running") != std::string::npos);              // the turn status survives truncation
    }
    TEST_MAIN_END();
}
