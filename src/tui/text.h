// Text helpers for the terminal UI: UTF-8, display width, word wrap, inline markdown, base64, OSC 52.
// Pure functions, no terminal access, so they are unit tested (tests/test_tui_text.cpp).
#pragma once
#include <string>
#include <vector>

namespace judas::tui {

// Colors and attributes as ready-to-print escape strings. An empty palette (plain()) prints no escapes.
struct Palette {
    std::string reset, bold, dim, accent, text, ok, warn, err, think, code, user;
    static Palette plain();
    static Palette from_skin(const std::string & skin_name);  // truecolor from the Judas skin
};

char32_t utf8_decode(const std::string & s, size_t i, size_t * len);
size_t utf8_next(const std::string & s, size_t i);  // start of the codepoint after i
size_t utf8_prev(const std::string & s, size_t i);  // start of the codepoint before i
int cp_width(char32_t cp);                          // 0, 1 or 2 terminal columns
int display_width(const std::string & s);           // columns, ignoring ANSI escape sequences
std::string strip_ansi(const std::string & s);
// Cut plain text to at most maxw columns, ending with "..." when something was cut.
std::string truncate_width(const std::string & s, int maxw);
std::string one_line(const std::string & s);        // newlines and tabs to spaces

struct WrapRow {
    std::string text;
    size_t start = 0;     // byte offset of the row's first byte in the input
    size_t consumed = 0;  // input bytes used through the end of this row (includes the break space or newline)
};
// Greedy word wrap of plain text to `width` columns. Newlines force a break. Long words are split.
// Always returns at least one row; text ending in a newline gets a trailing empty row.
std::vector<WrapRow> wrap(const std::string & s, int width);

// Inline markdown state carried across the wrapped rows of one logical line.
struct MdState {
    bool in_fence = false;  // inside a ``` block (persists across lines)
    bool bold = false, code = false;  // per line
};
// Render one logical markdown line into wrapped, styled rows. The first row gets first_prefix, the rest
// cont_prefix (both plain text, their width is subtracted from `width`).
std::vector<std::string> render_md_line(const std::string & line, int width, MdState & st, const Palette & pal,
                                        const std::string & first_prefix, const std::string & cont_prefix);

std::string base64_encode(const std::string & data);
// "\033]52;c;<base64>\033\\" (ST terminator; works in foot, kitty, alacritty, ghostty, tmux with passthrough).
std::string osc52(const std::string & text);

std::string human_tokens(long long n);  // 1234 -> "1.2k"

}  // namespace judas::tui
