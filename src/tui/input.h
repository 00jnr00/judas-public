// Keyboard input for the terminal UI: a byte-stream key decoder (with bracketed paste), a multiline editor
// with paste placeholders, and persisted history. Pure, unit tested (tests/test_tui_input.cpp).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace judas::tui {

struct Key {
    enum Type { Char, Enter, Tab, BackTab, Backspace, Delete, Up, Down, Left, Right, Home, End, PgUp, PgDn, Esc, Paste };
    Type type = Char;
    std::string text;  // Char: the UTF-8 character; Paste: the pasted text
    bool shift = false, alt = false, ctrl = false;
    // Ctrl+<letter> arrives as type Char with ctrl set and text = the lowercase letter.
    bool is_ctrl(char c) const { return type == Char && ctrl && text.size() == 1 && text[0] == c; }
};

class KeyDecoder {
public:
    // Feed raw bytes; returns the keys completed so far. A lone ESC stays pending until flush_esc() (timeout).
    std::vector<Key> feed(const std::string & bytes);
    std::vector<Key> flush_esc();
    bool pending() const { return !buf_.empty(); }

private:
    std::string buf_;
    std::string paste_;
    bool in_paste_ = false;
    std::vector<Key> drain();
};

// Escape sequences to enable/disable: bracketed paste, kitty keyboard flag 1 and xterm modifyOtherKeys.
const char * terminal_input_enable();
const char * terminal_input_disable();

struct PastePolicy {
    int min_lines = 4;      // placeholder when the paste has at least this many lines ...
    size_t min_chars = 400;  // ... or at least this many characters
};

class Editor {
public:
    std::string text;
    size_t cur = 0;

    void insert(const std::string & s);                 // typed text, \r\n normalised
    void paste(const std::string & s, const PastePolicy & p = {});  // big pastes become "[Pasted N lines]"
    void newline() { insert("\n"); }
    void left(); void right(); void home(); void end();
    void word_left(); void word_right();
    bool up(); bool down();                              // false when already on the first / last line
    void backspace(); void del();
    void kill_to_eol(); void kill_to_bol(); void kill_word();
    void clear();
    void set(const std::string & s) { text = s; cur = s.size(); }
    bool empty() const { return text.empty(); }
    std::string expanded() const;                        // placeholders replaced by their content
    int line_count() const;
    // Cursor position as (row, column) of the logical lines, column in codepoints.
    std::pair<int, int> row_col() const;
    size_t line_start(size_t pos) const;
    size_t line_end(size_t pos) const;

private:
    std::vector<std::pair<std::string, std::string>> pastes_;  // placeholder, content
    bool erase_placeholder_before();
};

class History {
public:
    explicit History(std::string path = "") : path_(std::move(path)) {}
    void load();
    void add(const std::string & entry);                 // dedups against the last entry, persists
    // Walk the history. prev() starts from `draft` (the unsent text) on first call.
    bool prev(const std::string & draft, std::string & out);
    bool next(std::string & out);
    void reset_cursor() { pos_ = -1; }
    const std::vector<std::string> & items() const { return items_; }

private:
    std::string path_;
    std::vector<std::string> items_;
    int pos_ = -1;
    std::string draft_;
};
// History file format: one entry per line, newlines inside an entry escaped as \n and backslash as \\.
std::string history_encode(const std::string & s);
std::string history_decode(const std::string & s);

}  // namespace judas::tui
