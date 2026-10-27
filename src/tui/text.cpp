#include "tui/text.h"

#include <algorithm>

#include "core/skin.h"

namespace judas::tui {

Palette Palette::plain() { return {}; }

static std::string rgb_fg(const Skin & s, const char * key) {
    int r, g, b;
    if (!Skin::parse_hex(s.color(key), r, g, b)) return "";
    return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

Palette Palette::from_skin(const std::string & name) {
    Skin s = skin_by_name(name.empty() ? "judas" : name);
    Palette p;
    p.reset = "\033[0m";
    p.bold = "\033[1m";
    p.dim = rgb_fg(s, "dim");
    p.accent = rgb_fg(s, "accent");
    p.text = rgb_fg(s, "text");
    p.ok = rgb_fg(s, "ok");
    p.warn = rgb_fg(s, "warn");
    p.err = rgb_fg(s, "error");
    p.think = rgb_fg(s, "thinking");
    p.code = rgb_fg(s, "title");
    p.user = "\033[1m" + rgb_fg(s, "title");
    return p;
}

char32_t utf8_decode(const std::string & s, size_t i, size_t * len) {
    unsigned char c = (unsigned char) s[i];
    size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) n = 1;
    char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
    if (n == 1) { *len = 1; return c; }
    for (size_t k = 1; k < n; k++) {
        unsigned char cc = (unsigned char) s[i + k];
        if ((cc & 0xc0) != 0x80) { *len = 1; return c; }
        cp = (cp << 6) | (cc & 0x3f);
    }
    *len = n;
    return cp;
}

size_t utf8_next(const std::string & s, size_t i) {
    if (i >= s.size()) return s.size();
    size_t n;
    utf8_decode(s, i, &n);
    return i + n;
}

size_t utf8_prev(const std::string & s, size_t i) {
    if (i == 0) return 0;
    size_t j = i - 1;
    while (j > 0 && ((unsigned char) s[j] & 0xc0) == 0x80) j--;
    return j;
}

int cp_width(char32_t cp) {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7f && cp < 0xa0)) return 0;
    if ((cp >= 0x300 && cp <= 0x36f) || (cp >= 0x200b && cp <= 0x200f) || (cp >= 0xfe00 && cp <= 0xfe0f) ||
        cp == 0x200d || (cp >= 0x20d0 && cp <= 0x20ff))
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
        (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff60) ||
        (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) || (cp >= 0x1f900 && cp <= 0x1f9ff) ||
        (cp >= 0x20000 && cp <= 0x3fffd))
        return 2;
    return 1;
}

std::string strip_ansi(const std::string & s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\033') {
            i++;
            if (i < s.size() && s[i] == '[') {
                i++;
                while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++;
                if (i < s.size()) i++;
            } else if (i < s.size() && s[i] == ']') {
                while (i < s.size() && s[i] != '\007' && !(s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '\\')) i++;
                i += (i < s.size() && s[i] == '\033') ? 2 : 1;
            } else if (i < s.size()) {
                i++;
            }
            continue;
        }
        o += s[i++];
    }
    return o;
}

int display_width(const std::string & s0) {
    std::string s = strip_ansi(s0);
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t n;
        w += cp_width(utf8_decode(s, i, &n));
        i += n;
    }
    return w;
}

std::string truncate_width(const std::string & s, int maxw) {
    if (maxw <= 0) return "";
    if (display_width(s) <= maxw) return s;
    int budget = std::max(0, maxw - 3), w = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t n;
        int cw = cp_width(utf8_decode(s, i, &n));
        if (w + cw > budget) break;
        w += cw;
        i += n;
    }
    return s.substr(0, i) + (maxw >= 3 ? "..." : "");
}

std::string one_line(const std::string & s) {
    std::string o = s;
    for (auto & c : o) if (c == '\n' || c == '\t' || c == '\r') c = ' ';
    return o;
}

std::vector<WrapRow> wrap(const std::string & s, int width) {
    if (width < 1) width = 1;
    std::vector<WrapRow> rows;
    const size_t n = s.size();
    size_t i = 0;
    bool ended_with_nl = false;
    while (i < n) {
        ended_with_nl = false;
        int w = 0;
        size_t j = i, last_space = std::string::npos;
        bool seen_non_space = false, pushed = false;
        while (j < n) {
            if (s[j] == '\n') {
                rows.push_back({s.substr(i, j - i), i, j + 1});
                i = j + 1;
                pushed = ended_with_nl = true;
                break;
            }
            size_t len;
            char32_t cp = utf8_decode(s, j, &len);
            int cw = cp_width(cp);
            if (w + cw > width && w > 0) {
                if (last_space != std::string::npos) {
                    size_t c = last_space + 1;
                    while (c < n && s[c] == ' ') c++;
                    rows.push_back({s.substr(i, last_space - i), i, c});
                    i = c;
                } else {
                    rows.push_back({s.substr(i, j - i), i, j});
                    i = j;
                }
                pushed = true;
                break;
            }
            if (cp == ' ') { if (seen_non_space) last_space = j; }
            else seen_non_space = true;
            w += cw;
            j += len;
        }
        if (!pushed) {
            rows.push_back({s.substr(i, n - i), i, n});
            i = n;
        }
    }
    if (rows.empty() || ended_with_nl) rows.push_back({"", n, n});
    return rows;
}

// Apply **bold** and `code` markers to one wrapped row, carrying state across rows of the same line.
static std::string style_row(const std::string & row, MdState & st, const Palette & pal) {
    std::string o;
    auto open = [&] {
        if (st.code) o += pal.code;
        else if (st.bold) o += pal.bold;
    };
    open();
    for (size_t i = 0; i < row.size();) {
        if (!st.code && row.compare(i, 2, "**") == 0) {
            st.bold = !st.bold;
            o += pal.reset;
            open();
            i += 2;
        } else if (row[i] == '`') {
            st.code = !st.code;
            o += pal.reset;
            if (st.bold && !st.code) o += pal.bold;
            open();
            i++;
        } else {
            o += row[i++];
        }
    }
    if (st.bold || st.code) o += pal.reset;
    return o;
}

std::vector<std::string> render_md_line(const std::string & line0, int width, MdState & st, const Palette & pal,
                                        const std::string & first_prefix, const std::string & cont_prefix) {
    std::vector<std::string> out;
    std::string line = line0;
    for (size_t p; (p = line.find('\t')) != std::string::npos;) line.replace(p, 1, "    ");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t ind = line.find_first_not_of(' ');
    std::string trimmed = ind == std::string::npos ? "" : line.substr(ind);
    int pw = display_width(first_prefix);
    auto emit = [&](const std::vector<WrapRow> & rows, const std::string & style, const std::string & hang,
                    bool markup) {
        st.bold = st.code = false;
        for (size_t k = 0; k < rows.size(); k++) {
            std::string body = markup ? style_row(rows[k].text, st, pal) : rows[k].text;
            std::string pre = out.empty() ? first_prefix : cont_prefix;
            out.push_back(pre + (k ? hang : "") + (markup ? "" : style) + body + (markup || style.empty() ? "" : pal.reset));
        }
    };
    if (trimmed.rfind("```", 0) == 0) {
        std::string lang = trimmed.substr(3);
        bool opening = !st.in_fence;
        st.in_fence = !st.in_fence;
        out.push_back(first_prefix + pal.dim + (opening ? (lang.empty() ? "```" : "``` " + lang) : "```") + pal.reset);
        return out;
    }
    if (st.in_fence) {
        for (auto & r : wrap(line, std::max(8, width - pw - 2)))
            out.push_back((out.empty() ? first_prefix : cont_prefix) + "  " + pal.code + r.text + pal.reset);
        return out;
    }
    if (trimmed.empty()) { out.push_back(""); return out; }
    if (trimmed[0] == '#') {
        size_t h = trimmed.find_first_not_of('#');
        if (h != std::string::npos && h <= 6 && trimmed[h] == ' ') {
            emit(wrap(trimmed.substr(h + 1), std::max(8, width - pw)), pal.bold + pal.accent, "", false);
            return out;
        }
    }
    if (trimmed[0] == '>') {
        std::string q = trimmed.size() > 1 && trimmed[1] == ' ' ? trimmed.substr(2) : trimmed.substr(1);
        emit(wrap(q, std::max(8, width - pw - 2)), pal.dim, "  ", false);
        for (size_t k = 0; k < out.size(); k++) {
            const std::string & pre = k == 0 ? first_prefix : cont_prefix;
            out[k] = pre + "| " + out[k].substr(pre.size());
        }
        return out;
    }
    // bullets and numbered items keep a hanging indent
    std::string lead(ind == std::string::npos ? 0 : ind, ' ');
    std::string body = trimmed, marker;
    if ((trimmed.size() > 2 && (trimmed[0] == '-' || trimmed[0] == '*' || trimmed[0] == '+') && trimmed[1] == ' ')) {
        marker = "\xE2\x80\xA2 ";
        body = trimmed.substr(2);
    } else {
        size_t d = 0;
        while (d < trimmed.size() && isdigit((unsigned char) trimmed[d])) d++;
        if (d > 0 && d < 4 && d + 1 < trimmed.size() && trimmed[d] == '.' && trimmed[d + 1] == ' ') {
            marker = trimmed.substr(0, d + 2);
            body = trimmed.substr(d + 2);
        }
    }
    std::string hang(display_width(marker), ' ');
    int avail = std::max(8, width - pw - (int) lead.size() - display_width(marker));
    auto rows = wrap(body, avail);
    st.bold = st.code = false;
    for (size_t k = 0; k < rows.size(); k++) {
        std::string pre = out.empty() ? first_prefix : cont_prefix;
        out.push_back(pre + lead + (k ? hang : marker) + style_row(rows[k].text, st, pal));
    }
    return out;
}

std::string base64_encode(const std::string & d) {
    static const char * T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < d.size(); i += 3) {
        unsigned v = ((unsigned char) d[i] << 16) | ((unsigned char) d[i + 1] << 8) | (unsigned char) d[i + 2];
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
    }
    if (i + 1 == d.size()) {
        unsigned v = (unsigned char) d[i] << 16;
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == d.size()) {
        unsigned v = ((unsigned char) d[i] << 16) | ((unsigned char) d[i + 1] << 8);
        o += T[v >> 18]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

std::string osc52(const std::string & text) { return "\033]52;c;" + base64_encode(text) + "\033\\"; }

std::string human_tokens(long long n) {
    if (n < 1000) return std::to_string(n);
    char b[32];
    if (n < 100000) std::snprintf(b, sizeof b, "%.1fk", n / 1000.0);
    else std::snprintf(b, sizeof b, "%lldk", n / 1000);
    return b;
}

}  // namespace judas::tui
