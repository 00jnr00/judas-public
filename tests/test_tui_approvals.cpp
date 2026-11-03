// TUI approvals: queue, request formatting (diff colors), pager, prompt line and the single-key state machine.
#include "tui/approvals.h"
#include "test.h"

using namespace judas;
using namespace judas::tui;

static Key ch(const std::string & t) { Key k; k.type = Key::Char; k.text = t; return k; }
static Key typ(Key::Type t) { Key k; k.type = t; return k; }
static bool has(const std::string & s, const std::string & n) { return s.find(n) != std::string::npos; }

static Palette colors() {
    Palette p;
    p.reset = "<R>"; p.bold = "<B>"; p.dim = "<D>"; p.ok = "<G>"; p.err = "<E>"; p.warn = "<W>";
    return p;
}

int main() {
    // parsing: an SSE frame and a list entry
    {
        json ev = {{"type", "approval_request"}, {"agent", "a-2"}, {"role", "worker"},
                   {"data", {{"id", "ap-7"}, {"tool", "edit_file"}, {"what", "edit_file /x\n-a\n+b"}, {"strict", true}, {"created_ms", 12345}}}};
        ApprovalReq r = approval_from_json(ev);
        CHECK(r.id == "ap-7" && r.tool == "edit_file" && r.agent == "a-2" && r.role == "worker" && r.strict && r.created_ms == 12345);
        json entry = {{"id", "ap-8"}, {"agent", "a-1"}, {"role", "lead"}, {"tool", "shell"}, {"what", "shell: ls"}, {"strict", false}, {"created_ms", 5}};
        ApprovalReq e = approval_from_json(entry);
        CHECK(e.id == "ap-8" && e.agent == "a-1" && !e.strict && e.tool == "shell");
        CHECK(approval_from_json(json::array()).id.empty());
        CHECK(approval_from_json(json{{"type", "done"}}).id.empty());
    }
    // queue: order, duplicates, remove, sync
    {
        ApprovalQueue q;
        CHECK(q.empty() && q.current() == nullptr);
        CHECK(q.add({"a", "", "", "t", "x"}));
        CHECK(q.add({"b", "", "", "t", "y"}));
        CHECK(!q.add({"a", "", "", "t", "x"}));
        CHECK(!q.add({"", "", "", "t", "x"}));
        CHECK(q.size() == 2 && q.current()->id == "a");
        CHECK(!q.remove("zz"));
        CHECK(q.remove("a") && q.current()->id == "b");
        q.add({"c", "", "", "t", "z"});
        q.sync({{"c", "", "", "t", "z"}, {"d", "", "", "t", "w"}});   // b gone on the daemon, d new
        CHECK(q.size() == 2 && q.items()[0].id == "c" && q.items()[1].id == "d");
        q.clear();
        CHECK(q.empty());
    }
    // formatting: header, diff colors, truncation hint, pager
    {
        Palette pal = colors();
        ApprovalReq r{"ap-1", "a-1", "lead", "edit_file", "edit_file /tmp/f.txt\n-old line\n+new line\n context", false, 0};
        auto body = approval_body_rows(r, pal, 60);
        REQUIRE(body.size() == 3);
        CHECK(has(body[0], "<E>") && has(body[0], "-old line"));
        CHECK(has(body[1], "<G>") && has(body[1], "+new line"));
        CHECK(!has(body[2], "<G>") && !has(body[2], "<E>") && has(body[2], "context"));
        auto rows = approval_rows(r, pal, 60);
        REQUIRE(rows.size() == 5);
        CHECK(has(rows[0], "approval needed") && has(rows[0], "edit_file") && has(rows[0], "lead a-1"));
        CHECK(has(rows[1], "edit_file /tmp/f.txt"));
        CHECK(!has(rows[0], "always asked"));

        std::string big = "write_file /big\n";
        for (int i = 0; i < 30; i++) big += "+line " + std::to_string(i) + (i < 29 ? "\n" : "");
        ApprovalReq b{"ap-2", "a-3", "worker", "write_file", big, true, 0};
        auto brows = approval_rows(b, pal, 60, 12);
        CHECK(brows.size() == 2 + 12 + 1);
        CHECK(has(brows.back(), "18 more lines") && has(brows.back(), "[v]"));
        CHECK(has(brows[0], "worker a-3") && has(brows[0], "always asked"));
        auto all = approval_body_rows(b, pal, 60);
        CHECK(all.size() == 30);
        size_t next = 0;
        auto p1 = approval_page(all, 0, 25, &next);
        CHECK(p1.size() == 25 && next == 25);
        auto p2 = approval_page(all, next, 25, &next);
        CHECK(p2.size() == 5 && next == 30);
        CHECK(approval_page(all, next, 25, &next).empty());

        // a long diff line wraps and keeps its color on every row
        ApprovalReq w{"ap-3", "a", "lead", "write_file", "write_file /f\n+" + std::string(100, 'x'), false, 0};
        auto wr = approval_body_rows(w, pal, 40);
        CHECK(wr.size() >= 3);
        for (auto & row : wr) CHECK(has(row, "<G>"));
        // a long one-line request (shell) shows its full text as the body
        ApprovalReq sh{"ap-4", "a", "lead", "shell", "shell: " + std::string(120, 'z'), false, 0};
        CHECK(approval_body_rows(sh, pal, 40).size() >= 3);
        ApprovalReq short1{"ap-5", "a", "lead", "shell", "shell: ls", false, 0};
        CHECK(approval_body_rows(short1, pal, 40).empty());
        // plain palette prints no escapes
        for (auto & row : approval_rows(r, Palette::plain(), 60)) CHECK(row.find('\033') == std::string::npos);
    }
    // prompt line
    {
        Palette pal = Palette::plain();
        CHECK(approval_prompt(pal, 0, false, 0, 0) == "  Approve? [y]es / [n]o / [v]iew full");
        CHECK(has(approval_prompt(pal, 2, false, 0, 0), "+2 waiting"));
        std::string v = approval_prompt(pal, 0, true, 25, 40);
        CHECK(has(v, "[space] more") && has(v, "25/40"));
    }
    // list rows
    {
        Palette pal = Palette::plain();
        ApprovalQueue q;
        CHECK(has(approval_list_rows(q, pal, 80)[0], "no approvals pending"));
        q.add({"ap-1", "a-1", "lead", "shell", "shell: ls -la\nmore", false, 0});
        q.add({"ap-2", "a-2", "worker", "edit_file", "edit_file /x", false, 0});
        auto rows = approval_list_rows(q, pal, 80);
        REQUIRE(rows.size() == 2);
        CHECK(has(rows[0], "> ap-1") && has(rows[0], "shell: ls -la") && !has(rows[0], "more"));
        CHECK(has(rows[1], "ap-2") && has(rows[1], "worker a-2"));
    }
    // keys: y n v space q Esc; Enter and everything else is swallowed
    {
        CHECK(approval_key(false, ch("y")) == ApprovalAct::Approve);
        CHECK(approval_key(false, ch("Y")) == ApprovalAct::Approve);
        CHECK(approval_key(false, ch("n")) == ApprovalAct::Deny);
        CHECK(approval_key(false, typ(Key::Esc)) == ApprovalAct::Deny);
        CHECK(approval_key(true, typ(Key::Esc)) == ApprovalAct::Deny);
        CHECK(approval_key(false, ch("v")) == ApprovalAct::ViewMore);
        CHECK(approval_key(true, ch("v")) == ApprovalAct::ViewMore);
        CHECK(approval_key(false, ch(" ")) == ApprovalAct::None);
        CHECK(approval_key(true, ch(" ")) == ApprovalAct::ViewMore);
        CHECK(approval_key(true, typ(Key::PgDn)) == ApprovalAct::ViewMore);
        CHECK(approval_key(false, typ(Key::PgDn)) == ApprovalAct::None);
        CHECK(approval_key(true, ch("q")) == ApprovalAct::StopView);
        CHECK(approval_key(false, ch("q")) == ApprovalAct::None);
        CHECK(approval_key(false, typ(Key::Enter)) == ApprovalAct::None);
        CHECK(approval_key(true, typ(Key::Enter)) == ApprovalAct::None);
        CHECK(approval_key(false, ch("a")) == ApprovalAct::None);
        CHECK(approval_key(false, typ(Key::Paste)) == ApprovalAct::None);
        Key cy = ch("y"); cy.ctrl = true;
        CHECK(approval_key(false, cy) == ApprovalAct::None);
        Key ay = ch("y"); ay.alt = true;
        CHECK(approval_key(false, ay) == ApprovalAct::None);
        CHECK(approval_key(false, typ(Key::Up)) == ApprovalAct::None);
    }
    TEST_MAIN_END();
}
