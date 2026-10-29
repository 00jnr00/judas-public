// Pixel-art companions for the terminal UI. Sprites come from the daemon (GET /sprites.json, the same file the
// web app draws); each 2 pixel rows become one terminal row with Unicode half blocks in truecolor. State comes
// from the event stream. Pure (no terminal access), unit tested in tests/test_tui_attach.cpp.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "core/json.h"

namespace judas::tui {

struct Pixel {
    bool on = false;
    int r = 0, g = 0, b = 0;
};
using SpriteFrame = std::vector<std::vector<Pixel>>;  // [y][x]

struct SpriteChar {
    std::string name;
    int w = 16, h = 16;
    std::map<std::string, std::vector<SpriteFrame>> frames;  // state -> animation frames
    std::map<std::string, double> fps;
};

// Two pixel rows per terminal row: "▀" with fg = top, bg = bottom; a transparent pixel uses the default colors.
// Each returned row is exactly `w` columns wide and ends with a reset.
std::vector<std::string> render_halfblock(const SpriteFrame & f, int w);

class Companions {
public:
    static constexpr double HAPPY_SECS = 3.0, SAD_SECS = 4.0;
    bool load(const judas::json & j);  // false when it holds no characters
    bool ready() const { return !chars_.empty(); }
    const std::vector<SpriteChar> & characters() const { return chars_; }

    // Events drive the state: reasoning = think; deltas and tool calls = work; a finished lead turn = happy
    // (HAPPY_SECS); an error or failed turn = sad (SAD_SECS).
    void on_event(const judas::json & ev, double now);
    void error(double now) { sad_until_ = now + SAD_SECS; happy_until_ = 0; }
    // idle think work happy sad; think/work fall back to idle when no turn is running.
    std::string state(double now, bool running) const;

    int panel_width() const;  // columns
    int sprite_rows() const;  // terminal rows of the tallest sprite
    int panel_rows() const { return sprite_rows() + 1; }  // plus the status line
    // panel_rows() rows, each exactly panel_width() columns: sprites side by side, then one status line.
    std::vector<std::string> render(double now, bool running, const std::string & dim, const std::string & reset) const;
    double frame_seconds(double now, bool running) const;  // time between frames in the current state

private:
    std::vector<SpriteChar> chars_;
    std::string last_ = "idle";
    double last_at_ = 0, happy_until_ = 0, sad_until_ = 0;
};

}  // namespace judas::tui
