// TUI text helpers: UTF-8 width, wrap, inline markdown, base64 and OSC 52.
#include "test.h"
#include "tui/text.h"

using namespace judas::tui;

int main() {
    // widths
    CHECK(display_width("abc") == 3);
    CHECK(display_width("\xE2\x97\x8F x") == 3);                 // circle is one column
    CHECK(display_width("\xE4\xBD\xA0\xE5\xA5\xBD") == 4);       // CJK is two columns each
    CHECK(display_width("\033[1mhi\033[0m") == 2);               // escapes do not count
    CHECK(strip_ansi("\033[38;2;1;2;3mred\033[0m") == "red");
    CHECK(truncate_width("hello world", 8) == "hello...");
    CHECK(truncate_width("hi", 8) == "hi");

    // wrap
    auto r = wrap("the quick brown fox", 10);
    REQUIRE(r.size() == 2);
    CHECK(r[0].text == "the quick");
    CHECK(r[1].text == "brown fox");
    CHECK(r[0].consumed == 10);
    auto lw = wrap("abcdefghij", 4);
    REQUIRE(lw.size() == 3);
    CHECK(lw[0].text == "abcd" && lw[1].text == "efgh" && lw[2].text == "ij");
    auto nl = wrap("a\nb\n", 10);
    REQUIRE(nl.size() == 3);                                      // trailing newline leaves an empty row
    CHECK(nl[2].text.empty());
    CHECK(wrap("", 10).size() == 1);
    // consuming rows then re-wrapping the rest gives the same rows (streaming relies on this)
    std::string long_text = "one two three four five six seven eight nine ten eleven twelve";
    auto full = wrap(long_text, 12);
    REQUIRE(full.size() > 3);
    auto rest = wrap(long_text.substr(full[1].consumed), 12);
    CHECK(rest[0].text == full[2].text);

    // markdown
    Palette plain = Palette::plain();
    MdState st;
    auto h = render_md_line("# Title", 40, st, plain, "", "");
    REQUIRE(h.size() == 1);
    CHECK(h[0] == "Title");
    auto b = render_md_line("- item with **bold** and `code`", 40, st, plain, "", "  ");
    REQUIRE(b.size() == 1);
    CHECK(b[0] == "\xE2\x80\xA2 item with bold and code");
    auto fence = render_md_line("```cpp", 40, st, plain, "", "");
    CHECK(st.in_fence);
    CHECK(strip_ansi(fence[0]) == "``` cpp");
    auto code = render_md_line("int x = **1**;", 40, st, plain, "", "");
    CHECK(code[0] == "  int x = **1**;");                        // no markup inside a fence
    render_md_line("```", 40, st, plain, "", "");
    CHECK(!st.in_fence);
    Palette col = Palette::from_skin("judas");
    MdState st2;
    auto styled = render_md_line("a **b** c", 40, st2, col, "", "");
    CHECK(styled[0].find("\033[1m") != std::string::npos);
    CHECK(strip_ansi(styled[0]) == "a b c");
    auto num = render_md_line("1. first item that is long enough to wrap around", 24, st2, plain, "", "");
    CHECK(num.size() >= 2);
    CHECK(num[1].rfind("   ", 0) == 0);                          // hanging indent

    // base64 / OSC 52
    CHECK(base64_encode("") == "");
    CHECK(base64_encode("f") == "Zg==");
    CHECK(base64_encode("fo") == "Zm8=");
    CHECK(base64_encode("foo") == "Zm9v");
    CHECK(base64_encode("foobar") == "Zm9vYmFy");
    CHECK(osc52("hi") == "\033]52;c;aGk=\033\\");
    CHECK(human_tokens(999) == "999" && human_tokens(1234) == "1.2k" && human_tokens(131072) == "131k");
    TEST_MAIN_END();
}
