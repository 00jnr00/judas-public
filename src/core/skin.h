// Skins: one palette, used by both the web app (as CSS variables) and the TUI (as terminal colors).
// A skin is a TOML file of named colors; the default is "judas" (silver, from the thirty pieces). Desktop
// themes can each ship a judas skin with the same keys. Keys are fixed so both faces stay in sync.
#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace judas {

struct Skin {
    std::string name = "judas";
    std::string label = "Judas";
    bool dark = true;
    // Hex colors (#rrggbb). The set is deliberately small; both faces understand every key.
    std::map<std::string, std::string> colors = {
        {"bg", "#0b0b0d"},        // app background, near black
        {"surface", "#16161b"},   // panels, cards
        {"surface2", "#1f1f26"},  // insets, input fields
        {"border", "#44444c"},    // tarnished silver
        {"title", "#e8e8f0"},     // bright silver, headings
        {"accent", "#c0c0c8"},    // silver, links and markers
        {"text", "#d7d7de"},      // bone white body text
        {"dim", "#878791"},       // muted text, reasoning, separators
        {"ok", "#6fae6f"},        // success
        {"warn", "#c3a74a"},      // warning
        {"error", "#b73131"},     // blood red, errors and irreversible actions
        {"thinking", "#70707a"},  // model reasoning
        {"added", "#2f5a34"},     // diff added
        {"removed", "#5c2626"},   // diff removed
    };

    // Optional design tokens ([tokens] table): radius, radius_sm, font_ui, font_mono, space. Defaults below;
    // a skin file may set any subset. Values are CSS strings.
    std::map<std::string, std::string> tokens = {
        {"radius", "8px"},
        {"radius_sm", "4px"},
        {"font_ui", "system-ui,-apple-system,\"Segoe UI\",sans-serif"},
        {"font_mono", "ui-monospace,\"SFMono-Regular\",Menlo,Consolas,monospace"},
        {"space", "8px"},
    };

    std::string color(const std::string & key) const;  // "" if absent
    std::string token(const std::string & key) const;  // "" if absent
    // "#rrggbb" -> r,g,b. Returns false on a bad value.
    static bool parse_hex(const std::string & hex, int & r, int & g, int & b);
};

// Parse one skin file's text (name falls back to <fallback_name>). Colors and tokens not set in the text keep the
// defaults. False on a malformed file.
bool parse_skin_toml(const std::string & text, const std::string & fallback_name, Skin & out);

// Built-in "judas" skin, then the themes embedded from the repo's skins/ folder, then *.toml files in <dirs> (default ~/.config/judas/skins). Keys in a file
// override the default's, so a skin may set only a few colors; a file with the same name as an earlier skin
// replaces it (user files beat the embedded ones). Never throws; a bad file is skipped.
std::vector<Skin> load_skins(const std::vector<std::string> & dirs = {"~/.config/judas/skins"});
Skin skin_by_name(const std::string & name, const std::vector<std::string> & dirs = {"~/.config/judas/skins"});

// :root { --bg: ...; --radius: ...; } plus a few derived helpers, for the web app's /skin.css. Every color is
// --<key>; tokens are --radius --radius-sm --font-ui --font-mono --space.
std::string skin_to_css(const Skin & s);

// {"name","label","dark","vars":{"--bg":"#..",...,"--radius":"8px",...}} for GET /api/skins/<name> and the
// postMessage theme protocol (docs/THEMES.md). vars has the same variables skin_to_css emits.
nlohmann::json skin_to_json(const Skin & s);

// The CSS variable map behind both of the above.
std::map<std::string, std::string> skin_vars(const Skin & s);

}  // namespace judas
