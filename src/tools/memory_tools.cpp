// Shared memory tools and the prompt block built from the shared memory file.
#include "tools/memory_tools.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>

namespace judas {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> split_lines(const std::string & t) {
    std::vector<std::string> out;
    std::istringstream is(t);
    std::string l;
    while (std::getline(is, l)) out.push_back(l);
    return out;
}

std::string join_lines(const std::vector<std::string> & v) {
    std::string o;
    for (auto & l : v) o += l + "\n";
    return o;
}

// "## Name" (not "###") -> Name, else "".
bool h2(const std::string & l, std::string * name = nullptr) {
    if (l.rfind("## ", 0) != 0) return false;
    if (name) *name = trim(l.substr(3));
    return true;
}

std::string read_file(const std::string & path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string today() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_r(&t, &lt);
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", &lt);
    return b;
}

// Truncate to at most n bytes on a UTF-8 boundary.
std::string cut_utf8(const std::string & s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

const std::string & path_of(const ToolContext & ctx) {
    static const Config def;
    static thread_local std::string p;
    p = expand_home((ctx.cfg ? *ctx.cfg : def).memory.path);
    return p;
}

ToolResult t_memory_read(const json & a, ToolContext & ctx) {
    std::string path = path_of(ctx);
    if (!fs::exists(path)) return {false, "memory file not found: " + path};
    std::string text = read_file(path);
    std::string sec = a.value("section", "");
    if (sec.empty()) {
        std::string o = "Sections in " + path + ":\n";
        for (auto & n : memory_section_names(text)) o += "- " + n + "\n";
        return {true, o};
    }
    std::string body = memory_section_text(text, sec);
    if (body.empty()) {
        std::string o = "no section named '" + sec + "'. Sections:";
        for (auto & n : memory_section_names(text)) o += " " + n + ";";
        return {false, o};
    }
    return {true, body};
}

ToolResult t_memory_search(const json & a, ToolContext & ctx) {
    std::string q = trim(a.value("query", ""));
    if (q.empty()) return {false, "query is empty"};
    std::string path = path_of(ctx);
    if (!fs::exists(path)) return {false, "memory file not found: " + path};
    auto hits = memory_search_text(read_file(path), q);
    if (hits.empty()) return {true, "no matches for '" + q + "'"};
    std::string o;
    for (auto & h : hits) o += h + "\n";
    return {true, o};
}

// Rewrite the file under the shared ".lock" flock with an atomic rename. `edit` gets the current text (""
// when missing) and returns the new text, or sets *err and returns "" to abort without writing.
ToolResult locked_rewrite(const std::string & path_in, const std::function<std::string(const std::string &, std::string *)> & edit) {
    std::error_code ec;
    // Write through a symlink to its target, so a linked (synced) memory file keeps receiving updates.
    std::string path = path_in;
    if (fs::is_symlink(path_in, ec)) {
        auto t = fs::canonical(path_in, ec);
        if (!ec) path = t.string();
        ec.clear();
    }
    fs::create_directories(fs::path(path).parent_path(), ec);
    int lockfd = ::open((path + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (lockfd < 0) return {false, "cannot lock " + path + ".lock"};
    ::flock(lockfd, LOCK_EX);
    ToolResult res{true, ""};
    // A file that exists but cannot be read in full must never be treated as empty: the rewrite would replace
    // the whole memory with one bullet.
    std::string text;
    if (fs::exists(path)) {
        std::ifstream in(path, std::ios::binary);
        std::error_code sec;
        auto want = fs::file_size(path, sec);
        std::stringstream ss;
        if (in) ss << in.rdbuf();
        text = ss.str();
        if (!in.good() && !in.eof()) text.clear();
        if (!in || sec || text.size() != want) {
            ::flock(lockfd, LOCK_UN);
            ::close(lockfd);
            return {false, "cannot read " + path + " in full; nothing was written"};
        }
    }
    std::string err;
    std::string out = edit(text, &err);
    if (!err.empty()) res = {false, err};
    std::string tmp = path + ".tmp." + std::to_string(getpid());
    if (res.ok) {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << out;
        if (!f) res = {false, "cannot write " + tmp};
    }
    if (res.ok) {
        struct stat st{};
        if (::stat(path.c_str(), &st) == 0) ::chmod(tmp.c_str(), st.st_mode & 07777);
        fs::rename(tmp, path, ec);
        if (ec) res = {false, "rename failed: " + ec.message()};
    }
    if (!res.ok) fs::remove(tmp, ec);
    ::flock(lockfd, LOCK_UN);
    ::close(lockfd);
    return res;
}

ToolResult t_memory_append(const json & a, ToolContext & ctx) {
    std::string section = trim(a.value("section", "")), fact = trim(a.value("fact", ""));
    if (section.empty() || fact.empty()) return {false, "section and fact are both required"};
    if (section.find('\n') != std::string::npos) return {false, "section must be one line"};
    std::string why = memory_check_fact(fact, ctx.cfg ? ctx.cfg->memory.reject_dashes : false);
    if (!why.empty()) return {false, why};
    for (auto & c : fact) if (c == '\n' || c == '\r') c = ' ';
    std::string line = "- " + today() + " [judas] " + fact;
    ToolResult res = locked_rewrite(path_of(ctx), [&](const std::string & text, std::string *) {
        return memory_insert_bullet(text, section, line);
    });
    if (res.ok) res.output = "added to " + section + ": " + line;
    return res;
}

// prompt block cache: path -> (mtime ns, file text)
std::mutex cache_mu;
std::map<std::string, std::pair<long long, std::string>> cache;

}  // namespace

std::vector<std::string> memory_section_names(const std::string & text) {
    std::vector<std::string> out;
    std::string n;
    for (auto & l : split_lines(text))
        if (h2(l, &n)) out.push_back(n);
    return out;
}

std::string memory_section_text(const std::string & text, const std::string & name) {
    std::string want = lower(trim(name)), n;
    std::vector<std::string> out;
    bool in = false;
    for (auto & l : split_lines(text)) {
        if (h2(l, &n)) {
            if (in) break;
            in = lower(n) == want;
        }
        if (in) out.push_back(l);
    }
    while (!out.empty() && trim(out.back()).empty()) out.pop_back();
    return join_lines(out);
}

std::string memory_insert_bullet(const std::string & text, const std::string & section, const std::string & line) {
    std::vector<std::string> ls = split_lines(text);
    std::string want = lower(trim(section)), n;
    int start = -1, end = (int) ls.size();
    for (int i = 0; i < (int) ls.size(); i++) {
        if (h2(ls[i], &n)) {
            if (start >= 0) { end = i; break; }
            if (lower(n) == want) start = i;
        }
    }
    if (start >= 0) {
        int at = end;   // after the last non-blank line of the section
        while (at > start + 1 && trim(ls[at - 1]).empty()) at--;
        ls.insert(ls.begin() + at, line);
        if (at == start + 1) ls.insert(ls.begin() + at, "");   // empty section: blank line under the heading
        return join_lines(ls);
    }
    // new section before "## Log", else at the end
    int log = -1;
    for (int i = 0; i < (int) ls.size(); i++)
        if (h2(ls[i], &n) && lower(n) == "log") { log = i; break; }
    std::vector<std::string> block = {"## " + trim(section), "", line, ""};
    if (log >= 0) {
        ls.insert(ls.begin() + log, block.begin(), block.end());
    } else {
        while (!ls.empty() && trim(ls.back()).empty()) ls.pop_back();
        if (!ls.empty()) ls.push_back("");
        block.pop_back();
        ls.insert(ls.end(), block.begin(), block.end());
    }
    return join_lines(ls);
}

std::vector<std::string> memory_search_text(const std::string & text, const std::string & query, size_t max) {
    std::vector<std::string> out;
    std::string q = lower(trim(query)), sec = "(top)", n;
    for (auto & l : split_lines(text)) {
        if (h2(l, &n)) { sec = n; continue; }
        if (l.rfind("- ", 0) == 0 && lower(l).find(q) != std::string::npos) {
            out.push_back("[" + sec + "] " + l);
            if (out.size() >= max) break;
        }
    }
    return out;
}

std::string memory_check_fact(const std::string & fact, bool reject_dashes) {
    if (!reject_dashes) return "";
    if (fact.find("\xE2\x80\x94") != std::string::npos || fact.find("\xE2\x80\x93") != std::string::npos)
        return "the fact contains an em dash or en dash; rewrite it without dashes (use a semicolon, a colon, or a "
               "comma with and, but, or so) and call memory_append again";
    return "";
}

std::string memory_remove_line(const std::string & text, const std::string & line, bool * removed) {
    std::vector<std::string> ls = split_lines(text);
    std::string want = trim(line);
    bool hit = false;
    for (size_t i = 0; i < ls.size(); i++) {
        if (trim(ls[i]) != want) continue;
        ls.erase(ls.begin() + (long) i);
        hit = true;
        break;
    }
    if (removed) *removed = hit;
    return hit ? join_lines(ls) : text;
}

bool memory_is_judas_line(const std::string & line) {
    static const std::regex re(R"(^- \d{4}-\d{2}-\d{2} \[judas\] )");
    return std::regex_search(trim(line) + " ", re);
}

std::string memory_forget(const Config & cfg, const std::string & line) {
    std::string l = trim(line);
    if (!memory_is_judas_line(l)) return "only bullets Judas wrote (\"- <date> [judas] ...\") can be removed here";
    if (cfg.memory.path.empty()) return "no memory file is configured";
    ToolResult r = locked_rewrite(expand_home(cfg.memory.path), [&](const std::string & text, std::string * err) {
        bool hit = false;
        std::string out = memory_remove_line(text, l, &hit);
        if (!hit) *err = "that line is no longer in the memory file (it may have been edited); refresh and try again";
        return out;
    });
    return r.ok ? "" : r.output;
}

std::string memory_prompt_block(const Config & cfg) {
    if (cfg.memory.inject_sections.empty() || cfg.memory.path.empty()) return "";
    std::string path = expand_home(cfg.memory.path);
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return "";
    long long mt = (long long) st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
    std::string text;
    {
        std::lock_guard lk(cache_mu);
        auto it = cache.find(path);
        if (it == cache.end() || it->second.first != mt) {
            cache[path] = {mt, read_file(path)};
            it = cache.find(path);
        }
        text = it->second.second;
    }
    std::string body;
    for (auto & s : cfg.memory.inject_sections) {
        std::string t = memory_section_text(text, s);
        if (!t.empty()) body += t + "\n\n";
    }
    if (body.empty()) return "";
    size_t cap = (size_t) std::max(0, cfg.prompt.max_memory_tokens) * 4;
    std::string head = "# Memory\nNotes about the user and how they work (file: " + cfg.memory.path +
                       "). Use memory_read, memory_search and memory_append for the rest.\n\n";
    if (body.size() > cap) body = cut_utf8(body, cap) + "\n[memory truncated; use memory_read for the full text]\n";
    return head + trim(body);
}

void register_memory_tools(ToolRegistry & reg) {
    auto strp = [](const char * d) { return json{{"type", "string"}, {"description", d}}; };
    Tool r;
    r.name = "memory_read";
    r.description = "Read the memory file: one section by name, or with no section the list of section names.";
    r.parameters = {{"type", "object"}, {"properties", {{"section", strp("Section name, e.g. Rules")}}}, {"required", json::array()}};
    r.run = t_memory_read;
    reg.add(std::move(r));
    Tool s;
    s.name = "memory_search";
    s.description = "Search the memory for a word or phrase; returns matching bullets with their section.";
    s.parameters = {{"type", "object"}, {"properties", {{"query", strp("Text to find (case-insensitive)")}}}, {"required", {"query"}}};
    s.run = t_memory_search;
    reg.add(std::move(s));
    Tool a;
    a.name = "memory_append";
    a.description = "Add one fact to the memory (one short sentence). It is written as a dated "
                    "bullet tagged [judas] at the end of the named section; a missing section is created. Only save "
                    "durable facts the user would be fine having written down; nothing about finances, legal matters or health.";
    a.parameters = {{"type", "object"},
                    {"properties", {{"section", strp("Section name, e.g. Log or Projects")}, {"fact", strp("The fact, one sentence")}}},
                    {"required", {"section", "fact"}}};
    a.run = t_memory_append;
    reg.add(std::move(a));
}

}  // namespace judas
