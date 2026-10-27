// Layout of the live area under the transcript: the bordered input box, the "/" and "@" menu, and the
// status line. Pure string building (no terminal access), unit tested in tests/test_tui_view.cpp.
#pragma once
#include <string>
#include <vector>

#include "tui/commands.h"
#include "tui/input.h"
#include "tui/text.h"

namespace judas::tui {

struct BoxView {
    std::vector<std::string> rows;  // top border, text rows, bottom border (each exactly `width` columns)
    int cursor_row = 0;             // within rows
    int cursor_col = 0;             // display column
};
// A rounded box around the editor text with a "> " prompt. At most max_text_rows text rows are shown
// (scrolled to keep the cursor visible).
BoxView render_input_box(const Editor & ed, int width, const Palette & pal, const std::string & placeholder,
                         bool shell_mode, int max_text_rows = 8);

// Menu rows (at most max_rows, scrolled around the selection): "  > /help   show keys and commands".
std::vector<std::string> render_menu(const Menu & m, int width, const Palette & pal, int max_rows = 8);

struct StatusInfo {
    std::string model, mode, session, turn;  // turn: idle running paused done failed killed
    long long kv_used = 0, kv_total = 0;
    int queued = 0, active = 0, pending_msgs = 0;
    double tok_s = 0;
    bool connected = true;
};
std::string render_status(const StatusInfo & s, int width, const Palette & pal);

}  // namespace judas::tui
