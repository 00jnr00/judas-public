#include "tui/sprites.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "tui/text.h"

namespace judas::tui {

namespace {
using json = judas::json;

bool parse_hex(const std::string & s, Pixel & p) {
    if (s.size() != 7 || s[0] != '#') return false;
    char * end = nullptr;
    long v = std::strtol(s.c_str() + 1, &end, 16);
    if (end != s.c_str() + 7) return false;
    p.on = true;
    p.r = (v >> 16) & 255;
    p.g = (v >> 8) & 255;
    p.b = v & 255;
    return true;
}

std::vector<std::string> rows_of(const json & f) {
    std::vector<std::string> r;
    if (f.is_string()) {
        std::string s = f.get<std::string>();
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t nl = s.find('\n', pos);
            r.push_back(s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    } else if (f.is_array()) {
        for (auto & x : f) {
            if (x.is_string()) r.push_back(x.get<std::string>());
            else if (x.is_array()) {  // a row given as an array of one-character strings
                std::string s;
                for (auto & c : x) s += c.is_string() ? c.get<std::string>() : std::string(".");
                r.push_back(s);
            }
        }
    }
    return r;
}

const char * HALF = "\xE2\x96\x80";  // upper half block

std::string fg(const Pixel & p) { return "\033[38;2;" + std::to_string(p.r) + ";" + std::to_string(p.g) + ";" + std::to_string(p.b) + "m"; }
std::string bg(const Pixel & p) { return "\033[48;2;" + std::to_string(p.r) + ";" + std::to_string(p.g) + ";" + std::to_string(p.b) + "m"; }
}  // namespace

std::vector<std::string> render_halfblock(const SpriteFrame & f, int w) {
    std::vector<std::string> out;
    Pixel none;
    for (size_t y = 0; y < f.size(); y += 2) {
        std::string row;
        for (int x = 0; x < w; x++) {
            const Pixel & t = x < (int) f[y].size() ? f[y][x] : none;
            const Pixel & b = y + 1 < f.size() && x < (int) f[y + 1].size() ? f[y + 1][x] : none;
            if (!t.on && !b.on) row += "\033[0m ";
            else if (t.on && !b.on) row += "\033[0m" + fg(t) + HALF;
            else if (!t.on && b.on) row += "\033[0m" + fg(b) + "\xE2\x96\x84";  // lower half block
            else row += fg(t) + bg(b) + HALF;
        }
        out.push_back(row + "\033[0m");
    }
    return out;
}

bool Companions::load(const json & j) {
    chars_.clear();
    if (!j.is_object() || !j.contains("characters") || !j["characters"].is_object()) return false;
    std::map<std::string, Pixel> pal;
    if (j.contains("palette") && j["palette"].is_object())
        for (auto it = j["palette"].begin(); it != j["palette"].end(); ++it) {
            Pixel p;
            if (it.value().is_string() && parse_hex(it.value().get<std::string>(), p)) pal[it.key()] = p;
        }
    for (auto it = j["characters"].begin(); it != j["characters"].end(); ++it) {
        const json & c = it.value();
        if (!c.is_object() || !c.contains("frames") || !c["frames"].is_object()) continue;
        SpriteChar sc;
        sc.name = it.key();
        sc.w = c.value("w", 16);
        sc.h = c.value("h", 16);
        for (auto st = c["frames"].begin(); st != c["frames"].end(); ++st) {
            std::vector<SpriteFrame> list;
            if (!st.value().is_array()) continue;
            for (auto & fr : st.value()) {
                SpriteFrame sf;
                for (auto & row : rows_of(fr)) {
                    std::vector<Pixel> px;
                    for (int x = 0; x < sc.w; x++) {
                        Pixel p;
                        if (x < (int) row.size()) {
                            auto f = pal.find(std::string(1, row[x]));
                            if (f != pal.end()) p = f->second;
                        }
                        px.push_back(p);
                    }
                    sf.push_back(px);
                }
                if (!sf.empty()) list.push_back(sf);
            }
            if (!list.empty()) sc.frames[st.key()] = list;
        }
        if (c.contains("fps") && c["fps"].is_object())
            for (auto f = c["fps"].begin(); f != c["fps"].end(); ++f)
                if (f.value().is_number()) sc.fps[f.key()] = f.value().get<double>();
        if (!sc.frames.empty()) chars_.push_back(sc);
    }
    return !chars_.empty();
}

void Companions::on_event(const json & ev, double now) {
    std::string type = ev.is_object() && ev.contains("type") && ev["type"].is_string() ? ev["type"].get<std::string>() : "";
    json d = ev.contains("data") && ev["data"].is_object() ? ev["data"] : json::object();
    bool lead = !ev.contains("parent") || !ev["parent"].is_string() || ev["parent"].get<std::string>().empty();
    auto set = [&](const char * s) { last_ = s; last_at_ = now; };
    if (type == "reasoning") set("think");
    else if (type == "delta" || type == "tool_call" || type == "tool_result" || type == "spawned") {
        if (type == "spawned" && !lead) return;
        happy_until_ = 0;
        set("work");
    } else if (type == "status" && d.contains("error")) {
        error(now);
    } else if (type == "done" && lead) {
        std::string st = d.contains("status") && d["status"].is_string() ? d["status"].get<std::string>() : "done";
        if (st == "done") { happy_until_ = now + HAPPY_SECS; sad_until_ = 0; }
        else if (st == "failed" || st == "killed") error(now);
        set("idle");
    }
}

std::string Companions::state(double now, bool running) const {
    if (now < sad_until_) return "sad";
    if (now < happy_until_) return "happy";
    if (running && (last_ == "think" || last_ == "work")) return last_;
    return "idle";
}

int Companions::panel_width() const {
    int w = 0;
    for (auto & c : chars_) w += c.w;
    if (!chars_.empty()) w += 2 * ((int) chars_.size() - 1);
    return w;
}

int Companions::sprite_rows() const {
    int h = 0;
    for (auto & c : chars_) h = std::max(h, (c.h + 1) / 2);
    return h;
}

static const std::vector<SpriteFrame> * frames_for(const SpriteChar & c, const std::string & st) {
    auto it = c.frames.find(st);
    if (it == c.frames.end()) it = c.frames.find("idle");
    if (it == c.frames.end()) it = c.frames.begin();
    return &it->second;
}

static double fps_for(const SpriteChar & c, const std::string & st) {
    auto it = c.fps.find(st);
    double f = it != c.fps.end() ? it->second : 3.0;
    return std::clamp(f, 0.5, 12.0);
}

double Companions::frame_seconds(double now, bool running) const {
    if (chars_.empty()) return 1.0;
    return 1.0 / fps_for(chars_[0], state(now, running));
}

std::vector<std::string> Companions::render(double now, bool running, const std::string & dim, const std::string & reset) const {
    std::string st = state(now, running);
    int pw = panel_width(), sr = sprite_rows();
    std::vector<std::string> rows(sr);
    for (size_t ci = 0; ci < chars_.size(); ci++) {
        const SpriteChar & c = chars_[ci];
        const auto * fl = frames_for(c, st);
        long idx = (long) std::floor(now * fps_for(c, st)) + (long) ci * 3;
        const SpriteFrame & f = (*fl)[(size_t) (((idx % (long) fl->size()) + (long) fl->size()) % (long) fl->size())];
        auto r = render_halfblock(f, c.w);
        for (int y = 0; y < sr; y++) {
            if (ci > 0) rows[y] += "  ";
            rows[y] += y < (int) r.size() ? r[y] : std::string(c.w, ' ');
        }
    }
    static const std::map<std::string, std::string> label = {
        {"idle", "resting"}, {"think", "thinking"}, {"work", "working"}, {"happy", "all done"}, {"sad", "something broke"}};
    std::string text = label.at(st);
    if ((int) text.size() > pw) text = truncate_width(text, pw);
    int pad = std::max(0, pw - (int) text.size());
    std::string status = std::string(pad / 2, ' ') + dim + text + reset + std::string(pad - pad / 2, ' ');
    rows.push_back(status);
    return rows;
}

}  // namespace judas::tui
