#include "tui/layout.h"

#include <algorithm>

namespace judas::tui {

namespace {
std::string repeat(const char * s, int n) {
    std::string o;
    for (int i = 0; i < n; i++) o += s;
    return o;
}
std::string pad_to(const std::string & s, int cols) {
    int w = display_width(s);
    return w >= cols ? s : s + std::string(cols - w, ' ');
}
}  // namespace

BoxView render_input_box(const Editor & ed, int width, const Palette & pal, const std::string & placeholder,
                         bool shell_mode, int max_text_rows) {
    BoxView v;
    if (width < 12) width = 12;
    const int inner = width - 4;       // "| " ... " |"
    const int tw = std::max(4, inner - 2);  // text columns after the "> " prompt
    const std::string & bc = shell_mode ? pal.warn : pal.dim;
    std::vector<std::string> trows;
    int cur_row = 0, cur_col = 0;
    // wrap each logical line, find the cursor
    size_t pos = 0;
    int logical = 0;
    auto [crow, ccol] = ed.row_col();
    (void) ccol;
    while (true) {
        size_t nl = ed.text.find('\n', pos);
        std::string line = ed.text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        auto rows = wrap(line, tw);
        for (size_t k = 0; k < rows.size(); k++) {
            if (logical == crow) {
                size_t off = ed.cur - pos;
                bool last = k + 1 == rows.size();
                if ((off >= rows[k].start && off < rows[k].consumed) || (last && off >= rows[k].start)) {
                    cur_row = (int) trows.size();
                    size_t take = std::min(off - rows[k].start, rows[k].text.size());
                    cur_col = display_width(rows[k].text.substr(0, take));
                }
            }
            trows.push_back(rows[k].text);
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
        logical++;
    }
    int total = (int) trows.size();
    int first = 0;
    if (total > max_text_rows) {
        first = std::clamp(cur_row - max_text_rows + 1, 0, total - max_text_rows);
        if (cur_row < first) first = cur_row;
    }
    int shown = std::min(total, max_text_rows);
    v.rows.push_back(bc + "\xE2\x95\xAD" + repeat("\xE2\x94\x80", width - 2) + "\xE2\x95\xAE" + pal.reset);
    for (int i = 0; i < shown; i++) {
        int idx = first + i;
        std::string body;
        if (ed.text.empty() && i == 0) body = pal.dim + placeholder + pal.reset;
        else body = pal.text + trows[idx] + pal.reset;
        std::string prompt = (idx == 0) ? pal.accent + pal.bold + (shell_mode ? "! " : "> ") + pal.reset : "  ";
        int used = 2 + (ed.text.empty() ? display_width(placeholder) : display_width(trows[idx]));
        v.rows.push_back(bc + "\xE2\x94\x82" + pal.reset + " " + prompt + body +
                         std::string(std::max(0, inner - used), ' ') + " " + bc + "\xE2\x94\x82" + pal.reset);
    }
    v.rows.push_back(bc + "\xE2\x95\xB0" + repeat("\xE2\x94\x80", width - 2) + "\xE2\x95\xAF" + pal.reset);
    v.cursor_row = 1 + (cur_row - first);
    v.cursor_col = 2 + 2 + cur_col;  // border, space, prompt, text
    return v;
}

std::vector<std::string> render_menu(const Menu & m, int width, const Palette & pal, int max_rows) {
    std::vector<std::string> o;
    if (!m.active()) return o;
    int n = (int) m.items.size();
    int rows = std::min(n, max_rows);
    int first = std::clamp(m.sel - rows / 2, 0, n - rows);
    int labw = 0;
    for (auto & it : m.items) labw = std::max(labw, display_width(it.label));
    labw = std::min(labw, width / 2);
    for (int i = first; i < first + rows; i++) {
        const auto & it = m.items[i];
        bool sel = i == m.sel;
        std::string lab = truncate_width(it.label, labw);
        std::string line = std::string(sel ? "  > " : "    ") + pad_to(lab, labw);
        if (!it.desc.empty()) line += "  " + truncate_width(it.desc, std::max(8, width - labw - 8));
        line = truncate_width(line, width - 1);
        o.push_back((sel ? pal.accent + pal.bold : pal.dim) + line + pal.reset);
    }
    if (n > rows) o.back() += pal.dim + "  (" + std::to_string(m.sel + 1) + "/" + std::to_string(n) + ")" + pal.reset;
    return o;
}

std::string render_status(const StatusInfo & s, int width, const Palette & pal) {
    std::vector<std::string> parts;
    parts.push_back(s.model.empty() ? "(model ?)" : s.model);
    parts.push_back(s.mode);
    if (s.kv_total > 0) parts.push_back("KV " + human_tokens(s.kv_used) + "/" + human_tokens(s.kv_total));
    parts.push_back("q " + std::to_string(s.queued) + (s.active ? " act " + std::to_string(s.active) : ""));
    if (s.tok_s > 0) {
        char b[32];
        std::snprintf(b, sizeof b, "%.1f tok/s", s.tok_s);
        parts.push_back(b);
    }
    parts.push_back(s.session.empty() ? "no session" : s.session.substr(0, 14));
    std::string line = " ";
    for (size_t i = 0; i < parts.size(); i++) line += (i ? " \xC2\xB7 " : "") + parts[i];
    std::string turn = s.turn.empty() ? "idle" : s.turn;
    if (!s.connected) turn = "offline";
    const std::string & tc = turn == "running" ? pal.accent : turn == "paused" ? pal.warn
                           : (turn == "failed" || turn == "offline") ? pal.err : turn == "done" ? pal.ok : pal.dim;
    std::string tail = " \xC2\xB7 " + turn + (s.pending_msgs ? " (+" + std::to_string(s.pending_msgs) + " queued)" : "");
    line = truncate_width(line, std::max(10, width - display_width(tail) - 1));
    return pal.dim + line + pal.reset + tc + tail + pal.reset;
}

}  // namespace judas::tui
