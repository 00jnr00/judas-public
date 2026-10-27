#include "tui/input.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "tui/text.h"

namespace judas::tui {

// ---- key decoding -------------------------------------------------------------------------------------------

static void apply_mods(Key & k, int m) {
    m -= 1;
    if (m < 0) return;
    k.shift = m & 1;
    k.alt = m & 2;
    k.ctrl = m & 4;
}

static Key from_codepoint(unsigned cp, int mods) {
    Key k;
    apply_mods(k, mods);
    switch (cp) {
        case 13: k.type = Key::Enter; return k;
        case 9: k.type = k.shift ? Key::BackTab : Key::Tab; return k;
        case 27: k.type = Key::Esc; return k;
        case 127: case 8: k.type = Key::Backspace; return k;
        default: break;
    }
    k.type = Key::Char;
    std::string t;
    if (cp < 0x80) t += (char) cp;
    else if (cp < 0x800) { t += (char) (0xc0 | (cp >> 6)); t += (char) (0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) { t += (char) (0xe0 | (cp >> 12)); t += (char) (0x80 | ((cp >> 6) & 0x3f)); t += (char) (0x80 | (cp & 0x3f)); }
    else { t += (char) (0xf0 | (cp >> 18)); t += (char) (0x80 | ((cp >> 12) & 0x3f)); t += (char) (0x80 | ((cp >> 6) & 0x3f)); t += (char) (0x80 | (cp & 0x3f)); }
    if (k.ctrl && cp >= 'A' && cp <= 'Z') t[0] = (char) (cp + 32);
    k.text = t;
    return k;
}

std::vector<Key> KeyDecoder::feed(const std::string & bytes) {
    buf_ += bytes;
    return drain();
}

std::vector<Key> KeyDecoder::flush_esc() {
    std::vector<Key> out;
    if (!in_paste_ && buf_ == "\033") {
        buf_.clear();
        Key k;
        k.type = Key::Esc;
        out.push_back(k);
    }
    return out;
}

std::vector<Key> KeyDecoder::drain() {
    std::vector<Key> out;
    size_t i = 0;
    while (i < buf_.size()) {
        if (in_paste_) {
            size_t e = buf_.find("\033[201~", i);
            if (e == std::string::npos) {
                // keep a possible partial terminator at the tail, the rest is paste content
                size_t keep = std::min<size_t>(5, buf_.size() - i);
                paste_ += buf_.substr(i, buf_.size() - i - keep);
                buf_.erase(0, buf_.size() - keep);
                return out;
            }
            paste_ += buf_.substr(i, e - i);
            Key k;
            k.type = Key::Paste;
            k.text = paste_;
            out.push_back(k);
            paste_.clear();
            in_paste_ = false;
            i = e + 6;
            continue;
        }
        unsigned char c = (unsigned char) buf_[i];
        Key k;
        if (c == 0x1b) {
            if (i + 1 >= buf_.size()) break;  // lone ESC: wait
            unsigned char n = (unsigned char) buf_[i + 1];
            if (n == '[') {
                size_t j = i + 2;
                while (j < buf_.size() && !(buf_[j] >= 0x40 && buf_[j] <= 0x7e)) j++;
                if (j >= buf_.size()) break;  // incomplete
                std::string params = buf_.substr(i + 2, j - i - 2);
                char fin = buf_[j];
                i = j + 1;
                if (params == "200" && fin == '~') { in_paste_ = true; buf_.erase(0, i); i = 0; paste_.clear(); continue; }
                std::vector<int> nums;
                {
                    std::string cur;
                    for (char ch : params + ";") {
                        if (ch == ';') { nums.push_back(cur.empty() ? 0 : std::atoi(cur.c_str())); cur.clear(); }
                        else if (ch == ':') { cur.clear(); }  // ignore kitty sub-fields (keep the first only below)
                        else cur += ch;
                    }
                }
                auto num = [&](size_t idx, int d) { return idx < nums.size() && nums[idx] ? nums[idx] : d; };
                if (fin == 'u') { out.push_back(from_codepoint(num(0, 0), num(1, 1))); continue; }
                if (fin == '~' && nums.size() >= 3 && nums[0] == 27) { out.push_back(from_codepoint(nums[2], nums[1])); continue; }
                int mods = nums.size() >= 2 ? nums[1] : 1;
                switch (fin) {
                    case 'A': k.type = Key::Up; break;
                    case 'B': k.type = Key::Down; break;
                    case 'C': k.type = Key::Right; break;
                    case 'D': k.type = Key::Left; break;
                    case 'H': k.type = Key::Home; break;
                    case 'F': k.type = Key::End; break;
                    case 'Z': k.type = Key::BackTab; k.shift = true; break;
                    case '~':
                        switch (num(0, 0)) {
                            case 1: case 7: k.type = Key::Home; break;
                            case 4: case 8: k.type = Key::End; break;
                            case 3: k.type = Key::Delete; break;
                            case 5: k.type = Key::PgUp; break;
                            case 6: k.type = Key::PgDn; break;
                            default: continue;
                        }
                        break;
                    default: continue;  // unknown sequence: drop it
                }
                if (fin != 'Z') apply_mods(k, mods);
                out.push_back(k);
                continue;
            }
            if (n == 'O') {  // SS3
                if (i + 2 >= buf_.size()) break;
                char f = buf_[i + 2];
                i += 3;
                switch (f) {
                    case 'A': k.type = Key::Up; break;
                    case 'B': k.type = Key::Down; break;
                    case 'C': k.type = Key::Right; break;
                    case 'D': k.type = Key::Left; break;
                    case 'H': k.type = Key::Home; break;
                    case 'F': k.type = Key::End; break;
                    default: continue;
                }
                out.push_back(k);
                continue;
            }
            // Alt + key: ESC then the key
            buf_.erase(0, i + 1);
            auto sub = drain();
            if (!sub.empty()) sub[0].alt = true;
            for (auto & x : sub) out.push_back(x);
            return out;
        }
        if (c == '\r') { k.type = Key::Enter; out.push_back(k); i++; continue; }
        if (c == '\n') { k.type = Key::Char; k.ctrl = true; k.text = "j"; out.push_back(k); i++; continue; }
        if (c == '\t') { k.type = Key::Tab; out.push_back(k); i++; continue; }
        if (c == 127 || c == 8) { k.type = Key::Backspace; out.push_back(k); i++; continue; }
        if (c < 32) { k.type = Key::Char; k.ctrl = true; k.text = std::string(1, (char) ('a' + c - 1)); out.push_back(k); i++; continue; }
        size_t len;
        utf8_decode(buf_, i, &len);
        if (i + len > buf_.size()) break;
        // an incomplete multi-byte sequence waits for more bytes
        unsigned char lead = c;
        size_t want = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
        if (buf_.size() - i < want) break;
        k.type = Key::Char;
        k.text = buf_.substr(i, len);
        out.push_back(k);
        i += len;
    }
    buf_.erase(0, i);
    return out;
}

const char * terminal_input_enable() { return "\033[?2004h\033[>1u\033[>4;1m"; }
const char * terminal_input_disable() { return "\033[>4;0m\033[<u\033[?2004l"; }

// ---- editor -------------------------------------------------------------------------------------------------

void Editor::insert(const std::string & s0) {
    std::string s;
    for (size_t i = 0; i < s0.size(); i++) {
        if (s0[i] == '\r') { s += '\n'; if (i + 1 < s0.size() && s0[i + 1] == '\n') i++; }
        else s += s0[i];
    }
    text.insert(cur, s);
    cur += s.size();
}

void Editor::paste(const std::string & s0, const PastePolicy & p) {
    std::string s;
    for (size_t i = 0; i < s0.size(); i++) {
        if (s0[i] == '\r') { s += '\n'; if (i + 1 < s0.size() && s0[i + 1] == '\n') i++; }
        else s += s0[i];
    }
    int lines = 1 + (int) std::count(s.begin(), s.end(), '\n');
    if (!s.empty() && s.back() == '\n') lines--;
    if (lines >= p.min_lines || s.size() >= p.min_chars) {
        std::string ph = "[Pasted " + std::to_string(lines) + (lines == 1 ? " line]" : " lines]");
        for (int n = 2;; n++) {
            bool taken = false;
            for (auto & pr : pastes_) taken = taken || pr.first == ph;
            if (!taken) break;
            ph = "[Pasted " + std::to_string(lines) + " lines #" + std::to_string(n) + "]";
        }
        pastes_.push_back({ph, s});
        text.insert(cur, ph);
        cur += ph.size();
    } else {
        text.insert(cur, s);
        cur += s.size();
    }
}

std::string Editor::expanded() const {
    std::string o = text;
    for (auto & [ph, content] : pastes_) {
        size_t p = o.find(ph);
        if (p != std::string::npos) o.replace(p, ph.size(), content);
    }
    return o;
}

void Editor::left() { cur = utf8_prev(text, cur); }
void Editor::right() { cur = utf8_next(text, cur); }
size_t Editor::line_start(size_t pos) const {
    size_t p = text.rfind('\n', pos == 0 ? std::string::npos : pos - 1);
    return p == std::string::npos || pos == 0 ? 0 : p + 1;
}
size_t Editor::line_end(size_t pos) const {
    size_t p = text.find('\n', pos);
    return p == std::string::npos ? text.size() : p;
}
void Editor::home() { cur = line_start(cur); }
void Editor::end() { cur = line_end(cur); }

static bool is_word(char c) { return std::isalnum((unsigned char) c) || (unsigned char) c >= 0x80 || c == '_'; }

void Editor::word_left() {
    while (cur > 0 && !is_word(text[cur - 1])) cur--;
    while (cur > 0 && is_word(text[cur - 1])) cur--;
}
void Editor::word_right() {
    while (cur < text.size() && !is_word(text[cur])) cur++;
    while (cur < text.size() && is_word(text[cur])) cur++;
}

bool Editor::up() {
    size_t ls = line_start(cur);
    if (ls == 0) return false;
    int col = (int) (cur - ls);
    size_t pe = ls - 1, ps = line_start(pe);
    cur = std::min(ps + col, pe);
    while (cur > ps && ((unsigned char) text[cur] & 0xc0) == 0x80) cur--;
    return true;
}
bool Editor::down() {
    size_t le = line_end(cur);
    if (le >= text.size()) return false;
    int col = (int) (cur - line_start(cur));
    size_t ns = le + 1, ne = line_end(ns);
    cur = std::min(ns + col, ne);
    while (cur > ns && cur < text.size() && ((unsigned char) text[cur] & 0xc0) == 0x80) cur--;
    return true;
}

bool Editor::erase_placeholder_before() {
    for (size_t i = 0; i < pastes_.size(); i++) {
        const std::string & ph = pastes_[i].first;
        if (cur >= ph.size() && text.compare(cur - ph.size(), ph.size(), ph) == 0) {
            text.erase(cur - ph.size(), ph.size());
            cur -= ph.size();
            pastes_.erase(pastes_.begin() + i);
            return true;
        }
    }
    return false;
}

void Editor::backspace() {
    if (cur == 0) return;
    if (erase_placeholder_before()) return;
    size_t p = utf8_prev(text, cur);
    text.erase(p, cur - p);
    cur = p;
}
void Editor::del() {
    if (cur >= text.size()) return;
    text.erase(cur, utf8_next(text, cur) - cur);
}
void Editor::kill_to_eol() {
    size_t e = line_end(cur);
    if (e == cur && e < text.size()) e++;  // at end of a line: join with the next
    text.erase(cur, e - cur);
}
void Editor::kill_to_bol() {
    size_t s = line_start(cur);
    if (s == cur && cur > 0) s--;  // at start of a line: join with the previous
    text.erase(s, cur - s);
    cur = s;
}
void Editor::kill_word() {
    size_t e = cur;
    while (e > 0 && !is_word(text[e - 1]) && text[e - 1] != '\n') e--;
    while (e > 0 && is_word(text[e - 1])) e--;
    text.erase(e, cur - e);
    cur = e;
}
void Editor::clear() { text.clear(); cur = 0; pastes_.clear(); }
int Editor::line_count() const { return 1 + (int) std::count(text.begin(), text.end(), '\n'); }
std::pair<int, int> Editor::row_col() const {
    int row = (int) std::count(text.begin(), text.begin() + cur, '\n');
    size_t ls = line_start(cur);
    int col = 0;
    for (size_t i = ls; i < cur;) { size_t n; utf8_decode(text, i, &n); i += n; col++; }
    return {row, col};
}

// ---- history ------------------------------------------------------------------------------------------------

std::string history_encode(const std::string & s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}
std::string history_decode(const std::string & s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            o += s[i + 1] == 'n' ? '\n' : s[i + 1];
            i++;
        } else {
            o += s[i];
        }
    }
    return o;
}

void History::load() {
    items_.clear();
    if (path_.empty()) return;
    std::ifstream f(path_);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) items_.push_back(history_decode(line));
    if (items_.size() > 1000) items_.erase(items_.begin(), items_.end() - 1000);
}

void History::add(const std::string & e) {
    pos_ = -1;
    if (e.empty() || (!items_.empty() && items_.back() == e)) return;
    items_.push_back(e);
    if (path_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream f(path_, std::ios::app);
    f << history_encode(e) << "\n";
}

bool History::prev(const std::string & draft, std::string & out) {
    if (items_.empty()) return false;
    if (pos_ < 0) { draft_ = draft; pos_ = (int) items_.size(); }
    if (pos_ == 0) return false;
    pos_--;
    out = items_[pos_];
    return true;
}
bool History::next(std::string & out) {
    if (pos_ < 0) return false;
    pos_++;
    if (pos_ >= (int) items_.size()) { pos_ = -1; out = draft_; return true; }
    out = items_[pos_];
    return true;
}

}  // namespace judas::tui
