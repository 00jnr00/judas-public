// Skins: one palette, used by both the web app (as CSS variables) and the TUI (as terminal colors).
// A skin is a TOML file of named colors; the default is "judas" (silver, from the thirty pieces). Desktop
// themes can each ship a judas skin with the same keys. Keys are fixed so both faces stay in sync.
#pragma once

#include <map>
#include <string>
#include <vector>

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
        {"dim", "#7f7f8a"},       // muted text, reasoning, separators
        {"ok", "#6fae6f"},        // success
        {"warn", "#c3a74a"},      // warning
        {"error", "#b22222"},     // blood red, errors and irreversible actions
        {"thinking", "#70707a"},  // model reasoning
        {"added", "#2f5a34"},     // diff added
        {"removed", "#5c2626"},   // diff removed
    };

    std::string color(const std::string & key) const;  // "" if absent
    // "#rrggbb" -> r,g,b. Returns false on a bad value.
    static bool parse_hex(const std::string & hex, int & r, int & g, int & b);
};

// Built-in "judas" skin plus *.toml files in <dirs> (default ~/.config/judas/skins). Keys in a file
// override the default's, so a skin may set only a few colors. Never throws; a bad file is skipped.
std::vector<Skin> load_skins(const std::vector<std::string> & dirs = {"~/.config/judas/skins"});
Skin skin_by_name(const std::string & name, const std::vector<std::string> & dirs = {"~/.config/judas/skins"});

// :root { --bg: ...; } plus a few derived helpers, for the web app's /skin.css.
std::string skin_to_css(const Skin & s);

}  // namespace judas
