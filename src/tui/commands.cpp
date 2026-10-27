#include "tui/commands.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace judas::tui {

const std::vector<Command> & commands() {
    static const std::vector<Command> c = {
        {"help", "", "show keys and commands"},
        {"new", "", "start a new session"},
        {"sessions", "", "pick a session and switch to it"},
        {"resume", "", "resume a paused turn"},
        {"cancel", "", "cancel the running turn"},
        {"mode", "chat|cowork|code", "set the session mode", true},
        {"model", "[name]", "list models or switch (restarts the daemon)"},
        {"agents", "", "live agent list"},
        {"plan", "", "show the current plan"},
        {"state", "", "daemon state: model, KV, queue, tok/s"},
        {"copy", "[N]", "copy the last assistant reply (or the Nth from last)"},
        {"expand", "[N]", "print the full output of tool call N (default: last)"},
        {"clear", "", "clear the screen"},
        {"auto-resume", "on|off", "auto-resume paused turns for this session", true},
        {"memory", "<query>", "search shared memory", true},
        {"learn", "[focus]", "save what this session taught as skills or memory", true},
        {"quit", "", "leave the TUI (the daemon keeps running)"},
    };
    return c;
}

const Command * find_command(const std::string & name) {
    for (auto & c : commands()) if (c.name == name) return &c;
    return nullptr;
}

static bool subsequence(const std::string & q, const std::string & s) {
    size_t k = 0;
    for (char c : s) if (k < q.size() && q[k] == c) k++;
    return k == q.size();
}

std::vector<const Command *> filter_commands(const std::string & q0) {
    std::string q = q0;
    for (auto & c : q) c = (char) std::tolower((unsigned char) c);
    std::vector<const Command *> pre, sub, fuzzy;
    for (auto & c : commands()) {
        if (q.empty() || c.name.rfind(q, 0) == 0) pre.push_back(&c);
        else if (c.name.find(q) != std::string::npos) sub.push_back(&c);
        else if (subsequence(q, c.name)) fuzzy.push_back(&c);
    }
    pre.insert(pre.end(), sub.begin(), sub.end());
    pre.insert(pre.end(), fuzzy.begin(), fuzzy.end());
    return pre;
}

static std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

Parsed parse_input(const std::string & text) {
    Parsed p;
    std::string t = trim(text);
    if (t.empty()) return p;
    if (t[0] == '!') {
        p.kind = Parsed::Shell;
        p.arg = trim(t.substr(1));
        if (p.arg.empty()) p.kind = Parsed::Empty;
        return p;
    }
    if (t[0] == '/' && t.size() > 1) {
        // "/name args": a path like /usr/bin/x has a second slash in the first word and is a message
        size_t sp = t.find_first_of(" \n");
        std::string word = t.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
        if (word.find('/') == std::string::npos) {
            p.kind = Parsed::Slash;
            for (auto & c : word) c = (char) std::tolower((unsigned char) c);
            p.name = word;
            p.arg = sp == std::string::npos ? "" : trim(t.substr(sp));
            return p;
        }
    }
    p.kind = Parsed::Message;
    p.arg = text;
    return p;
}

std::vector<std::string> complete_path(const std::string & cwd, const std::string & partial, size_t max) {
    std::vector<std::string> out;
    std::string dir_part, base;
    size_t sl = partial.rfind('/');
    if (sl == std::string::npos) base = partial;
    else { dir_part = partial.substr(0, sl + 1); base = partial.substr(sl + 1); }
    fs::path root = dir_part.empty() ? fs::path(cwd) : (dir_part[0] == '/' ? fs::path(dir_part) : fs::path(cwd) / dir_part);
    std::error_code ec;
    std::vector<std::pair<std::string, bool>> ents;
    for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), e; !ec && it != e; it.increment(ec)) {
        std::string name = it->path().filename().string();
        if (name.empty() || (name[0] == '.' && base.rfind('.', 0) != 0)) continue;
        if (name == "node_modules" && base.empty()) continue;
        bool dir = it->is_directory(ec);
        ents.push_back({name, dir});
    }
    auto lower = [](std::string s) { for (auto & c : s) c = (char) std::tolower((unsigned char) c); return s; };
    std::string lb = lower(base);
    std::vector<std::pair<std::string, bool>> pre, sub;
    for (auto & e : ents) {
        std::string ln = lower(e.first);
        if (ln.rfind(lb, 0) == 0) pre.push_back(e);
        else if (!lb.empty() && ln.find(lb) != std::string::npos) sub.push_back(e);
    }
    auto order = [](const auto & a, const auto & b) { return a.second != b.second ? a.second > b.second : a.first < b.first; };
    std::sort(pre.begin(), pre.end(), order);
    std::sort(sub.begin(), sub.end(), order);
    pre.insert(pre.end(), sub.begin(), sub.end());
    for (auto & e : pre) {
        if (out.size() >= max) break;
        out.push_back(dir_part + e.first + (e.second ? "/" : ""));
    }
    return out;
}

Menu compute_menu(const std::string & text, size_t cursor, const MenuContext & ctx) {
    Menu m;
    if (cursor > text.size()) cursor = text.size();
    // "@path" token under the cursor
    size_t ts = cursor;
    while (ts > 0 && !std::isspace((unsigned char) text[ts - 1])) ts--;
    if (ts < text.size() && text[ts] == '@' && cursor > ts) {
        std::string partial = text.substr(ts + 1, cursor - ts - 1);
        for (auto & p : complete_path(ctx.cwd, partial, 8)) m.items.push_back({"@" + p, p, ""});
        if (!m.items.empty()) { m.kind = Menu::At; m.begin = ts; m.end = cursor; }
        return m;
    }
    if (text.empty() || text[0] != '/' || text.find('\n') != std::string::npos) return m;
    size_t sp = text.find(' ');
    if (sp == std::string::npos || cursor <= sp) {
        if (text.find(' ') != std::string::npos) return m;  // cursor inside the word of a longer line: no menu
        for (auto * c : filter_commands(text.substr(1)))
            m.items.push_back({"/" + c->name + (c->needs_arg ? " " : ""),
                               "/" + c->name + (c->args_hint.empty() ? "" : " " + c->args_hint), c->desc});
        if (!m.items.empty()) { m.kind = Menu::Slash; m.begin = 0; m.end = text.size(); }
        return m;
    }
    // argument choices
    std::string name = text.substr(1, sp - 1), arg = trim(text.substr(sp + 1));
    std::vector<std::string> choices;
    if (name == "mode") choices = {"chat", "cowork", "code"};
    else if (name == "auto-resume") choices = {"on", "off"};
    else if (name == "model") choices = ctx.models;
    for (auto & c : choices)
        if (arg.empty() || c.rfind(arg, 0) == 0 || (name == "model" && c.find(arg) != std::string::npos))
            m.items.push_back({"/" + name + " " + c, c, ""});
    if (m.items.size() == 1 && m.items[0].label == arg) m.items.clear();  // already complete
    if (!m.items.empty()) { m.kind = Menu::Arg; m.begin = 0; m.end = text.size(); }
    return m;
}

}  // namespace judas::tui
