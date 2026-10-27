// TUI input: key decoding (CSI, kitty, bracketed paste), the multiline editor, paste placeholders, history.
#include <unistd.h>

#include <filesystem>

#include "test.h"
#include "tui/input.h"

using namespace judas::tui;

int main() {
    {   // plain keys, arrows, ctrl, alt
        KeyDecoder d;
        auto k = d.feed("a\x1b[A\x1b[1;5C\x01\x1b" "b\r");
        REQUIRE(k.size() == 6);
        CHECK(k[0].type == Key::Char && k[0].text == "a");
        CHECK(k[1].type == Key::Up);
        CHECK(k[2].type == Key::Right && k[2].ctrl);
        CHECK(k[3].is_ctrl('a'));
        CHECK(k[4].type == Key::Char && k[4].alt && k[4].text == "b");
        CHECK(k[5].type == Key::Enter && !k[5].alt);
    }
    {   // Shift+Enter in kitty and modifyOtherKeys forms, Alt+Enter, Ctrl+C as CSI u
        KeyDecoder d;
        auto k = d.feed("\x1b[13;2u\x1b[27;2;13~\x1b\r\x1b[99;5u");
        REQUIRE(k.size() == 4);
        CHECK(k[0].type == Key::Enter && k[0].shift);
        CHECK(k[1].type == Key::Enter && k[1].shift);
        CHECK(k[2].type == Key::Enter && k[2].alt);
        CHECK(k[3].is_ctrl('c'));
    }
    {   // lone ESC waits, then flushes; a split CSI completes on the next feed
        KeyDecoder d;
        CHECK(d.feed("\x1b").empty());
        CHECK(d.pending());
        auto k = d.flush_esc();
        REQUIRE(k.size() == 1);
        CHECK(k[0].type == Key::Esc);
        CHECK(d.feed("\x1b[").empty());
        auto k2 = d.feed("B");
        REQUIRE(k2.size() == 1);
        CHECK(k2[0].type == Key::Down);
    }
    {   // UTF-8 split across reads
        KeyDecoder d;
        CHECK(d.feed("\xE2\x82").empty());
        auto k = d.feed("\xAC");
        REQUIRE(k.size() == 1);
        CHECK(k[0].text == "\xE2\x82\xAC");
    }
    {   // bracketed paste, including a split terminator and embedded newlines
        KeyDecoder d;
        CHECK(d.feed("x\x1b[200~line1\nline2\x1b[2").size() == 1);   // only 'x' so far
        auto k = d.feed("01~y");
        REQUIRE(k.size() == 2);
        CHECK(k[0].type == Key::Paste);
        CHECK(k[0].text == "line1\nline2");
        CHECK(k[1].type == Key::Char && k[1].text == "y");
    }
    {   // editor: insert, move, kill
        Editor e;
        e.insert("hello world");
        e.word_left();
        CHECK(e.cur == 6);
        e.kill_word();
        CHECK(e.text == "world");
        e.end();
        e.insert("!");
        e.home();
        e.kill_to_eol();
        CHECK(e.text.empty());
        e.insert("abc\ndef");
        CHECK(e.line_count() == 2);
        CHECK(e.up());
        CHECK(e.row_col() == std::make_pair(0, 3));
        CHECK(!e.up());
        CHECK(e.down());
        e.kill_to_bol();
        CHECK(e.text == "abc\n");
        e.backspace();
        CHECK(e.text == "abc");
        Editor u;
        u.insert("h\xC3\xA9llo");
        u.left(); u.left(); u.left();
        u.backspace();                                             // removes the two-byte e-acute
        CHECK(u.text == "hllo");
    }
    {   // paste: small stays inline, big becomes a placeholder, expanded on send
        Editor e;
        e.insert("see: ");
        e.paste("one\ntwo");
        CHECK(e.text == "see: one\ntwo");
        std::string big;
        for (int i = 0; i < 12; i++) big += "line " + std::to_string(i) + "\n";
        e.clear();
        e.paste(big);
        CHECK(e.text == "[Pasted 12 lines]");
        e.insert(" please");
        CHECK(e.expanded() == big + " please");
        e.paste(big);
        CHECK(e.text.find("[Pasted 12 lines #2]") != std::string::npos);
        // backspace removes a whole placeholder
        Editor f;
        f.paste(big);
        f.backspace();
        CHECK(f.text.empty());
        Editor g;
        g.paste("a\r\nb\r\nc\r\nd\r\ne");
        CHECK(g.text == "[Pasted 5 lines]");
        CHECK(g.expanded() == "a\nb\nc\nd\ne");
        Editor h;
        h.paste(std::string(500, 'x'));                             // long single line also collapses
        CHECK(h.text == "[Pasted 1 line]");
    }
    {   // history: persistence, dedupe, walking with a draft
        std::string path = JUDAS_TEST_TMP "/judas-tui-hist-" + std::to_string(getpid());
        std::filesystem::remove(path);
        {
            History h(path);
            h.add("first");
            h.add("multi\nline \\ x");
            h.add("multi\nline \\ x");
        }
        History h(path);
        h.load();
        REQUIRE(h.items().size() == 2);
        CHECK(h.items()[1] == "multi\nline \\ x");
        std::string o;
        CHECK(h.prev("draft", o) && o == "multi\nline \\ x");
        CHECK(h.prev("draft", o) && o == "first");
        CHECK(!h.prev("draft", o));
        CHECK(h.next(o) && o == "multi\nline \\ x");
        CHECK(h.next(o) && o == "draft");
        CHECK(!h.next(o));
        std::filesystem::remove(path);
    }
    TEST_MAIN_END();
}
