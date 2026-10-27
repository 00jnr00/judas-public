// Slash commands, the "/" and "@" menus, path completion and shell-mode parsing for the terminal UI.
// Pure (path completion reads the directory), unit tested in tests/test_tui_commands.cpp.
#pragma once
#include <string>
#include <vector>

namespace judas::tui {

struct Command {
    std::string name;       // without the slash
    std::string args_hint;  // "chat|cowork|code", "" when none
    std::string desc;
    bool needs_arg = false; // Enter completes to "/name " instead of running
};
const std::vector<Command> & commands();
const Command * find_command(const std::string & name);
// Commands whose name starts with, contains, or fuzzy-matches `query` (prefix matches first).
std::vector<const Command *> filter_commands(const std::string & query);

struct Parsed {
    enum Kind { Message, Slash, Shell, Empty } kind = Empty;
    std::string name;  // Slash: command name, lowercase
    std::string arg;   // Slash: the rest, trimmed. Shell: the command line. Message: the text.
};
Parsed parse_input(const std::string & text);

struct MenuItem {
    std::string value;  // text that replaces the token on completion
    std::string label;
    std::string desc;
};
struct Menu {
    enum Kind { None, Slash, Arg, At, Picker } kind = None;
    std::vector<MenuItem> items;
    int sel = 0;
    size_t begin = 0, end = 0;  // byte range of the buffer the chosen value replaces
    bool active() const { return kind != None && !items.empty(); }
};
struct MenuContext {
    std::string cwd;
    std::vector<std::string> models;
};
// The menu implied by the editor text and cursor ("/" at the start, "/cmd " arguments, "@path" token).
Menu compute_menu(const std::string & text, size_t cursor, const MenuContext & ctx);
// Files and directories matching `partial` (a path as typed after "@"), relative to cwd, dirs end in '/'.
std::vector<std::string> complete_path(const std::string & cwd, const std::string & partial, size_t max = 50);

}  // namespace judas::tui
