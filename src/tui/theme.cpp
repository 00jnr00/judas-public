#include "tui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/config.h"

namespace judas::tui {

namespace fs = std::filesystem;

static std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

std::vector<ThemeInfo> list_themes(const std::vector<Skin> & skins) {
    std::vector<ThemeInfo> out;
    for (auto & s : skins) out.push_back({s.name, s.label.empty() ? s.name : s.label, s.dark});
    return out;
}

std::string resolve_theme(const std::vector<Skin> & skins, const std::string & query) {
    std::string q = lower(query);
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.empty()) return "";
    for (auto & s : skins) if (lower(s.name) == q) return s.name;
    for (auto & s : skins) if (lower(s.label) == q) return s.name;
    std::string hit;
    int n = 0;
    for (auto & s : skins)
        if (lower(s.name).rfind(q, 0) == 0) { hit = s.name; n++; }
    return n == 1 ? hit : "";
}

std::string tui_prefs_path() { return expand_home("~/.config/judas/tui.toml"); }

// The value of a `theme = "x"` line (quotes optional), or the line index/range for rewriting.
static bool theme_line(const std::string & line, std::string * value) {
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line.compare(a, 5, "theme") != 0) return false;
    size_t i = a + 5;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line.size() || line[i] != '=') return false;
    i++;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    std::string v = line.substr(i);
    size_t hash = v.find('#');
    if (v.find('"') == std::string::npos && hash != std::string::npos) v = v.substr(0, hash);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
        size_t e = v.find(v.front(), 1);
        v = v.substr(1, e == std::string::npos ? std::string::npos : e - 1);
    }
    if (value) *value = v;
    return true;
}

std::string parse_theme_pref(const std::string & text) {
    std::istringstream in(text);
    std::string line, v;
    while (std::getline(in, line)) {
        size_t a = line.find_first_not_of(" \t");
        if (a != std::string::npos && line[a] == '[') break;  // only the top-level table is ours
        if (theme_line(line, &v)) return v;
    }
    return "";
}

std::string with_theme_pref(const std::string & text, const std::string & theme) {
    std::istringstream in(text);
    std::string line, out, v;
    bool done = false, in_table = false;
    std::vector<std::string> lines;
    while (std::getline(in, line)) lines.push_back(line);
    const std::string mine = "theme = \"" + theme + "\"";
    std::vector<std::string> res;
    for (auto & l : lines) {
        size_t a = l.find_first_not_of(" \t");
        if (a != std::string::npos && l[a] == '[') {
            if (!done) { res.push_back(mine); done = true; }
            in_table = true;
        }
        if (!in_table && !done && theme_line(l, &v)) { res.push_back(mine); done = true; continue; }
        res.push_back(l);
    }
    if (!done) {
        // no top-level theme line and no table: append
        res.push_back(mine);
    }
    for (auto & l : res) out += l + "\n";
    return out;
}

std::string load_theme_pref(const std::string & path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return parse_theme_pref(ss.str());
}

bool save_theme_pref(const std::string & path, const std::string & theme) {
    std::string cur;
    {
        std::ifstream f(path);
        if (f) { std::stringstream ss; ss << f.rdbuf(); cur = ss.str(); }
    }
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::trunc);
        if (!o) return false;
        o << with_theme_pref(cur, theme);
        if (!o) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

// ---- colors ----------------------------------------------------------------------------------------------

static double chan(int v) {
    double c = v / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const std::string & hex) {
    int r, g, b;
    if (!Skin::parse_hex(hex, r, g, b)) return -1;
    return 0.2126 * chan(r) + 0.7152 * chan(g) + 0.0722 * chan(b);
}

double contrast_ratio(const std::string & a, const std::string & b) {
    double la = luminance(a), lb = luminance(b);
    if (la < 0 || lb < 0) return 0;
    if (la < lb) std::swap(la, lb);
    return (la + 0.05) / (lb + 0.05);
}

static std::string hex_of(int r, int g, int b) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
    return buf;
}

std::string ensure_contrast(const std::string & fg, const std::string & bg, double min_ratio) {
    int r, g, b;
    if (!Skin::parse_hex(fg, r, g, b)) return fg;
    if (luminance(bg) < 0 || contrast_ratio(fg, bg) >= min_ratio) return fg;
    // away from the background: lighten on a dark one, darken on a light one
    bool to_white = luminance(bg) < 0.4;
    int tr = to_white ? 255 : 0;
    std::string best = fg;
    for (int step = 1; step <= 20; step++) {
        double t = step / 20.0;
        std::string c = hex_of((int) std::lround(r + (tr - r) * t), (int) std::lround(g + (tr - g) * t),
                               (int) std::lround(b + (tr - b) * t));
        best = c;
        if (contrast_ratio(c, bg) >= min_ratio) break;
    }
    return best;
}

std::string terminal_bg_from_colorfgbg(const char * value) {
    if (!value || !*value) return "";
    std::string v = value;
    size_t p = v.rfind(';');
    std::string bg = p == std::string::npos ? v : v.substr(p + 1);
    if (bg.empty() || bg.find_first_not_of("0123456789") != std::string::npos) return "";
    int n = std::atoi(bg.c_str());
    if (n == 7 || n == 15) return "#ffffff";   // light backgrounds in the 16-color table
    if (n >= 0 && n <= 8) return "#000000";
    return "";
}

static std::string fg_of(const std::string & hex) {
    int r, g, b;
    if (!Skin::parse_hex(hex, r, g, b)) return "";
    return "\033[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

ThemeLook look_for(const Skin & s, const std::string & term_bg) {
    // the background the text will really sit on: the terminal's when known, else the skin's own
    std::string bg = !term_bg.empty() ? term_bg : s.color("bg");
    if (luminance(bg) < 0) bg = s.dark ? "#000000" : "#ffffff";
    auto pick = [&](const char * key, double min) { return fg_of(ensure_contrast(s.color(key), bg, min)); };
    ThemeLook l;
    Palette & p = l.pal;
    p.reset = "\033[0m";
    p.bold = "\033[1m";
    p.dim = pick("dim", 3.0);
    p.accent = pick("accent", 4.5);
    p.text = pick("text", 4.5);
    p.ok = pick("ok", 4.5);
    p.warn = pick("warn", 4.5);
    p.err = pick("error", 4.5);
    p.think = pick("thinking", 3.0);
    p.code = pick("title", 4.5);
    p.user = "\033[1m" + pick("title", 4.5);
    // diff colors: the skin's added/removed are subtle web backgrounds (dark green, dark red); as terminal
    // foreground they wash out to grey after the contrast nudge, so the skin's ok/error (its own green and red
    // text colors) carry the diff, falling back to added/removed when a skin has no ok/error
    l.diff.add = pick(s.color("ok").empty() ? "added" : "ok", 4.5);
    l.diff.del = pick(s.color("error").empty() ? "removed" : "error", 4.5);
    l.diff.hunk = pick("accent", 4.5);
    l.diff.head = "\033[1m" + pick("title", 4.5);
    l.diff.ctx = pick("dim", 3.0);
    if (!s.dark) {
        bool known_light = term_bg == "#ffffff";
        if (!known_light)
            l.warning = std::string("light theme: only text colors change (no background is painted); ") +
                        (term_bg.empty() ? "if your terminal is dark, colors are lightened to stay readable"
                                         : "your terminal looks dark, so colors are lightened to stay readable");
    }
    return l;
}

}  // namespace judas::tui
