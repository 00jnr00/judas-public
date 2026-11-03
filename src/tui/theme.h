// Live theme switching for the terminal UI: the skin list for the picker, the TUI-side preference file
// (~/.config/judas/tui.toml, never config.toml), and a readable Palette built from a skin.
//
// Light themes: the TUI never paints a full background. Only foreground colors are used, so a light skin on
// a dark terminal cannot produce a white slab. Because the terminal's real background is unknown, each
// foreground color is nudged until it has enough contrast against the assumed background: the terminal's own
// (from COLORFGBG when set) or else the skin's. Pure functions, unit tested in tests/test_tui_theme.cpp.
#pragma once
#include <string>
#include <vector>

#include "core/skin.h"
#include "tui/text.h"

namespace judas::tui {

struct ThemeInfo {
    std::string name, label;
    bool dark = true;
};
std::vector<ThemeInfo> list_themes(const std::vector<Skin> & skins);
// A theme name as typed: exact name, exact label (case-insensitive), then a unique prefix. "" when none or ambiguous.
std::string resolve_theme(const std::vector<Skin> & skins, const std::string & query);

// ---- preference file: a flat TOML, only `theme = "name"` is ours; other lines survive a rewrite.
std::string tui_prefs_path();  // ~/.config/judas/tui.toml
std::string parse_theme_pref(const std::string & toml_text);                               // "" when unset
std::string with_theme_pref(const std::string & toml_text, const std::string & theme);    // set or replace the line
std::string load_theme_pref(const std::string & path);
bool save_theme_pref(const std::string & path, const std::string & theme);

// ---- colors
double luminance(const std::string & hex);                               // WCAG relative luminance, -1 on a bad hex
double contrast_ratio(const std::string & a, const std::string & b);     // 1..21, 0 on a bad hex
// `fg` moved toward white or black (whichever helps) until it reaches `min_ratio` against `bg`.
std::string ensure_contrast(const std::string & fg, const std::string & bg, double min_ratio);
// "#rrggbb" for the terminal background from a COLORFGBG value ("15;0", "0;default;15"), "" when unknown.
std::string terminal_bg_from_colorfgbg(const char * value);

struct DiffColors {
    std::string add, del, hunk, head, ctx;  // ready-to-print escape strings (foreground only)
};
struct ThemeLook {
    Palette pal;
    DiffColors diff;
    std::string warning;  // non-empty when a light theme meets a dark (or unknown) terminal; shown once on apply
};
// `term_bg` = terminal_bg_from_colorfgbg(...) or "". Never sets a background color.
ThemeLook look_for(const Skin & s, const std::string & term_bg);

}  // namespace judas::tui
