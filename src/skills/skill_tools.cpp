// skill_manage implementation. Every write is versioned, checked against the write policy, journaled first,
// then the in-memory Skills index is reloaded so the next turn sees the change.
#include "skills/skill_tools.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "skills/skills.h"
#include "skills/usage.h"

namespace judas {
namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxBody = 16 * 1024;
constexpr size_t kMaxDescription = 200;
constexpr size_t kMaxName = 64;

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool valid_name(const std::string & n) {
    if (n.empty() || n.size() > kMaxName) return false;
    if (!(std::islower((unsigned char)n[0]) || std::isdigit((unsigned char)n[0]))) return false;
    for (char c : n)
        if (!(std::islower((unsigned char)c) || std::isdigit((unsigned char)c) || c == '-')) return false;
    return true;
}

std::string read_whole(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string write_whole(const fs::path & p, const std::string & content) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return "cannot open for writing: " + p.string();
    f << content;
    if (!f) return "write failed: " + p.string();
    return "";
}

std::string join_triggers(const std::vector<std::string> & triggers) {
    std::string out;
    for (const auto & t : triggers) {
        if (!out.empty()) out += ", ";
        out += t;
    }
    return out;
}

// Opening "---" plus the newline before the closing "---". False for a plain Markdown body.
bool frontmatter_span(const std::string & text, size_t & fm_begin, size_t & close) {
    if (text.rfind("---", 0) != 0) return false;
    close = text.find("\n---", 3);
    if (close == std::string::npos) return false;
    fm_begin = 3;
    return true;
}

std::string header_for(const Skill & s, int version) {
    std::string h = "---\n";
    h += "name: " + s.name + "\n";
    h += "description: " + s.description + "\n";
    if (!s.triggers.empty()) h += "triggers: [" + join_triggers(s.triggers) + "]\n";
    h += "source: " + (s.source.empty() ? std::string("local") : s.source) + "\n";
    if (s.pinned) h += "pinned: true\n";
    h += "version: " + std::to_string(version) + "\n---\n\n";
    return h;
}

std::string ensure_frontmatter(const std::string & text, const Skill & s, int version) {
    size_t begin = 0, close = 0;
    if (frontmatter_span(text, begin, close)) return text;
    return header_for(s, version) + text;
}

// Replace or insert a key in the frontmatter. Content without frontmatter is returned unchanged.
std::string set_field(const std::string & text, const std::string & key, const std::string & value) {
    size_t begin = 0, close = 0;
    if (!frontmatter_span(text, begin, close)) return text;
    std::string fm = text.substr(begin, close - begin);
    std::stringstream ss(fm);
    std::string out, line;
    bool first = true, found = false;
    while (std::getline(ss, line)) {
        if (first) {
            first = false;
            out = line;
            continue;
        }
        std::string t = trim(line);
        if (t.rfind(key + ":", 0) == 0) {
            out += "\n" + key + ": " + value;
            found = true;
        } else {
            out += "\n" + line;
        }
    }
    if (!found) out += "\n" + key + ": " + value;
    return text.substr(0, begin) + out + text.substr(close);
}

std::string bump_version(std::string text, const Skill & s, int version) {
    text = ensure_frontmatter(text, s, s.version);
    return set_field(text, "version", std::to_string(version));
}

// Writes need the same policy and journal treatment as write_file.
std::string prepare_write(ToolContext & ctx, const std::string & what, const std::string & path, fs::path & out) {
    std::string resolved;
    std::string why = check_write_path(ctx, path, resolved);
    if (!why.empty()) return "refused: " + why;
    out = resolved;
    const std::string mode = ctx.cfg ? ctx.cfg->permissions.mode : "full-auto";
    if (mode == "suggest") {
        if (!ctx.approve) return "not approved: " + what + " needs approval and no approver is available";
        if (!ctx.approve(what)) return "not approved: " + what + " was declined";
    }
    if (ctx.journal) ctx.journal->record(ctx.checkpoint, out);
    return "";
}

std::string require_name(const json & a, const std::string & action) {
    std::string name = a.value("name", "");
    if (name.empty()) return "name is required for action '" + action + "'";
    return "";
}

// An autonomous review has no user to consent. It may only change skills the agent itself created and that
// nobody pinned, so a lesson can never edit a hand-written skill behind the user's back.
std::string autonomous_refusal(const ToolContext & ctx, const Skill & s) {
    if (!ctx.autonomous) return "";
    if (s.pinned || s.source != "agent")
        return "refused: skill '" + s.name + "' is protected: user owned; an autonomous review cannot change it";
    return "";
}

ToolResult fail(const std::string & m) { return {false, m, false}; }
ToolResult okr(const std::string & m) { return {true, m, false}; }

ToolResult do_list(const Skills & skills) {
    const auto & all = skills.all();
    if (all.empty()) return okr("no skills");
    std::ostringstream o;
    for (const auto & s : all) {
        o << "- " << s.name << " (v" << s.version << ", " << s.source;
        if (s.pinned) o << ", pinned";
        o << "): " << s.description;
        if (!s.triggers.empty()) o << " [triggers: " << join_triggers(s.triggers) << "]";
        o << "\n";
    }
    return okr(o.str());
}

ToolResult do_view(const Skills & skills, const std::string & name) {
    auto s = skills.by_name(name);
    if (!s) return fail("no such skill: " + name);
    std::ostringstream o;
    o << "# skill: " << s->name << "\n";
    o << "source: " << s->source << "\n";
    o << "version: " << s->version << "\n";
    o << "pinned: " << (s->pinned ? "true" : "false") << "\n";
    if (!s->triggers.empty()) o << "triggers: " << join_triggers(s->triggers) << "\n";
    o << "\n" << s->body;
    return okr(o.str());
}

ToolResult do_create(const json & a, ToolContext & ctx, Skills & skills, SkillUsage * usage) {
    std::string name = a.value("name", "");
    if (!valid_name(name))
        return fail("invalid skill name '" + name +
                    "': use lowercase letters, digits and hyphens, start with a letter or digit, max 64 chars");
    std::string description = trim(a.value("description", ""));
    if (description.empty()) return fail("description is required for action 'create'");
    if (description.size() > kMaxDescription)
        return fail("description is " + std::to_string(description.size()) + " chars; the limit is " +
                    std::to_string(kMaxDescription));
    if (description.find('\n') != std::string::npos || description.find('\r') != std::string::npos)
        return fail("description must be one line");
    std::string body = a.value("body", "");
    if (body.size() > kMaxBody)
        return fail("body is " + std::to_string(body.size()) + " bytes; the limit is " + std::to_string(kMaxBody));

    if (skills.by_name(name)) return fail("skill '" + name + "' already exists");
    std::string dir = skills.write_dir();
    if (dir.empty()) return fail("no skills path is configured");
    fs::path path = fs::path(dir) / (name + ".md");
    std::error_code ec;
    if (fs::exists(path, ec)) return fail("skill '" + name + "' already exists at " + path.string());

    Skill s;
    s.name = name;
    s.description = description;
    s.source = "agent";
    s.version = 1;
    if (a.contains("triggers") && a["triggers"].is_array())
        for (const auto & t : a["triggers"])
            if (t.is_string()) {
                std::string v = trim(t.get<std::string>());
                // a trigger is written inside the frontmatter's [ ... ] list: no line breaks, brackets or commas
                if (v.find_first_of("\r\n[],") != std::string::npos)
                    return fail("a trigger may not contain line breaks, brackets or commas: " + v.substr(0, 60));
                if (!v.empty()) s.triggers.push_back(v);
            }

    fs::path resolved;
    std::string e = prepare_write(ctx, "skill_manage create " + name, path.string(), resolved);
    if (!e.empty()) return fail(e);
    std::string content = header_for(s, 1) + body;
    if (std::string w = write_whole(resolved, content); !w.empty()) return fail(w);
    skills.reload();
    if (usage) usage->record_create(name);
    return okr("created skill '" + name + "' (v1) at " + resolved.string());
}

ToolResult do_patch(const json & a, ToolContext & ctx, Skills & skills, SkillUsage * usage) {
    std::string name = a.value("name", "");
    if (std::string e = require_name(a, "patch"); !e.empty()) return fail(e);
    auto s = skills.by_name(name);
    if (!s) return fail("no such skill: " + name);
    if (std::string e = autonomous_refusal(ctx, *s); !e.empty()) return fail(e);
    if (s->pinned) return fail("skill '" + name + "' is pinned; unpin it before patching");
    std::string oldS = a.value("old_string", ""), newS = a.value("new_string", "");
    if (oldS.empty()) return fail("old_string is required for action 'patch'");
    if (oldS == newS) return fail("old_string and new_string are identical");

    fs::path path = s->path;
    std::string data = read_whole(path);
    size_t count = 0;
    for (size_t pos = data.find(oldS); pos != std::string::npos; pos = data.find(oldS, pos + oldS.size())) count++;
    if (count == 0) return fail("old_string not found in skill '" + name + "' (it must match exactly)");
    if (count > 1)
        return fail("old_string occurs " + std::to_string(count) + " times in skill '" + name +
                    "'; add surrounding context to make it unique");
    size_t pos = data.find(oldS);
    data.replace(pos, oldS.size(), newS);
    int version = s->version + 1;
    data = bump_version(std::move(data), *s, version);

    fs::path resolved;
    std::string e = prepare_write(ctx, "skill_manage patch " + name, path.string(), resolved);
    if (!e.empty()) return fail(e);
    if (std::string w = write_whole(resolved, data); !w.empty()) return fail(w);
    skills.reload();
    if (usage) usage->record_patch(name);
    return okr("patched skill '" + name + "' (v" + std::to_string(version) + ")");
}

ToolResult do_rewrite(const json & a, ToolContext & ctx, Skills & skills, SkillUsage * usage) {
    std::string name = a.value("name", "");
    if (std::string e = require_name(a, "rewrite"); !e.empty()) return fail(e);
    auto s = skills.by_name(name);
    if (!s) return fail("no such skill: " + name);
    if (std::string e = autonomous_refusal(ctx, *s); !e.empty()) return fail(e);
    if (s->pinned) return fail("skill '" + name + "' is pinned; unpin it before rewriting");
    std::string body = a.value("body", "");
    if (body.size() > kMaxBody)
        return fail("body is " + std::to_string(body.size()) + " bytes; the limit is " + std::to_string(kMaxBody));

    fs::path path = s->path;
    std::string data = read_whole(path);
    size_t begin = 0, close = 0;
    if (frontmatter_span(data, begin, close)) {
        size_t dashes_end = close + 4;   // "\n---"
        size_t nl = data.find('\n', dashes_end);
        std::string header = nl == std::string::npos ? data.substr(0, dashes_end) : data.substr(0, nl + 1);
        data = header + "\n" + body;
    } else {
        data = header_for(*s, s->version) + body;
    }
    int version = s->version + 1;
    data = bump_version(std::move(data), *s, version);

    fs::path resolved;
    std::string e = prepare_write(ctx, "skill_manage rewrite " + name, path.string(), resolved);
    if (!e.empty()) return fail(e);
    if (std::string w = write_whole(resolved, data); !w.empty()) return fail(w);
    skills.reload();
    if (usage) usage->record_patch(name);
    return okr("rewrote skill '" + name + "' (v" + std::to_string(version) + ")");
}

ToolResult do_archive(const json & a, ToolContext & ctx, Skills & skills) {
    std::string name = a.value("name", "");
    if (std::string e = require_name(a, "archive"); !e.empty()) return fail(e);
    auto s = skills.by_name(name);
    if (!s) return fail("no such skill: " + name);
    if (std::string e = autonomous_refusal(ctx, *s); !e.empty()) return fail(e);
    if (s->pinned) return fail("skill '" + name + "' is pinned; unpin it before archiving");
    std::string dir = skills.write_dir();
    if (dir.empty()) return fail("no skills path is configured");

    std::string src_resolved, dst_resolved;
    std::string why = check_write_path(ctx, s->path, src_resolved);
    if (!why.empty()) return fail("refused: " + why);
    fs::path dest = fs::path(dir) / ".archive" / (name + ".md");
    // an earlier archived generation keeps its file (the curator stamps collisions the same way)
    for (int n = 2; fs::exists(dest) && n < 1000; n++) dest = fs::path(dir) / ".archive" / (name + "." + std::to_string(n) + ".md");
    why = check_write_path(ctx, dest.string(), dst_resolved);
    if (!why.empty()) return fail("refused: " + why);
    const std::string mode = ctx.cfg ? ctx.cfg->permissions.mode : "full-auto";
    if (mode == "suggest") {
        if (!ctx.approve) return fail("not approved: skill_manage archive needs approval and no approver is available");
        if (!ctx.approve("skill_manage archive " + name)) return fail("not approved: skill_manage archive was declined");
    }
    if (ctx.journal) {
        ctx.journal->record(ctx.checkpoint, src_resolved);
        ctx.journal->record(ctx.checkpoint, dst_resolved);
    }
    // Archiving is a write too: keep the version chain monotonic.
    std::string data = bump_version(read_whole(src_resolved), *s, s->version + 1);
    if (std::string w = write_whole(src_resolved, data); !w.empty()) return fail(w);

    std::error_code ec;
    fs::create_directories(fs::path(dst_resolved).parent_path(), ec);
    fs::rename(src_resolved, dst_resolved, ec);
    if (ec) {
        std::error_code ce;
        fs::copy_file(src_resolved, dst_resolved, fs::copy_options::overwrite_existing, ce);
        if (ce) return fail("cannot archive skill '" + name + "': " + ce.message());
        fs::remove(src_resolved, ec);
        if (ec) return fail("cannot remove archived skill source '" + name + "': " + ec.message());
    }
    skills.reload();
    return okr("archived skill '" + name + "' to " + dst_resolved);
}

ToolResult do_pin(const json & a, ToolContext & ctx, Skills & skills, bool pinned) {
    std::string action = pinned ? "pin" : "unpin";
    std::string name = a.value("name", "");
    if (std::string e = require_name(a, action); !e.empty()) return fail(e);
    auto s = skills.by_name(name);
    if (!s) return fail("no such skill: " + name);
    if (std::string e = autonomous_refusal(ctx, *s); !e.empty()) return fail(e);
    if (s->pinned == pinned) return okr("skill '" + name + "' is already " + action + "ned");

    fs::path path = s->path;
    std::string data = read_whole(path);
    int version = s->version + 1;
    data = ensure_frontmatter(std::move(data), *s, s->version);
    data = set_field(data, "pinned", pinned ? "true" : "false");
    data = set_field(data, "version", std::to_string(version));

    fs::path resolved;
    std::string e = prepare_write(ctx, "skill_manage " + action + " " + name, path.string(), resolved);
    if (!e.empty()) return fail(e);
    if (std::string w = write_whole(resolved, data); !w.empty()) return fail(w);
    skills.reload();
    return okr(std::string(pinned ? "pinned" : "unpinned") + " skill '" + name + "' (v" + std::to_string(version) + ")");
}

ToolResult run_skill_manage(const json & a, ToolContext & ctx, Skills & skills, SkillUsage * usage) {
    std::string action = a.value("action", "");
    if (action == "list") return do_list(skills);
    if (action == "view") {
        std::string name = a.value("name", "");
        if (std::string e = require_name(a, "view"); !e.empty()) return fail(e);
        return do_view(skills, name);
    }
    if (action == "create") return do_create(a, ctx, skills, usage);
    if (action == "patch") return do_patch(a, ctx, skills, usage);
    if (action == "rewrite") return do_rewrite(a, ctx, skills, usage);
    if (action == "archive") return do_archive(a, ctx, skills);
    if (action == "pin") return do_pin(a, ctx, skills, true);
    if (action == "unpin") return do_pin(a, ctx, skills, false);
    return fail("unknown action '" + action + "': use list, view, create, patch, rewrite, archive, pin or unpin");
}

}  // namespace

void register_skill_tools(ToolRegistry & reg, Skills & skills, SkillUsage * usage) {
    Tool t;
    t.name = "skill_manage";
    t.description =
        "Create and maintain reusable skills (Markdown instructions). Actions: list, view, create, patch, rewrite, "
        "archive, pin, unpin. Writes are versioned, journaled and reloaded immediately.";
    t.parameters = {
        {"type", "object"},
        {"properties",
         {{"action", {{"type", "string"},
                      {"enum", {"list", "view", "create", "patch", "rewrite", "archive", "pin", "unpin"}}}},
          {"name", {{"type", "string"}, {"description", "Skill name: lowercase letters, digits and hyphens"}}},
          {"description", {{"type", "string"}, {"description", "One line, max 200 chars (create)"}}},
          {"triggers", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Autoload words (create)"}}},
          {"body", {{"type", "string"}, {"description", "Markdown body, max 16 KB (create, rewrite)"}}},
          {"old_string", {{"type", "string"}, {"description", "Exact unique text to replace (patch)"}}},
          {"new_string", {{"type", "string"}, {"description", "Replacement text (patch)"}}}}},
        {"required", {"action"}}};
    t.writes = true;
    t.run = [&skills, usage](const json & a, ToolContext & ctx) {
        return run_skill_manage(a, ctx, skills, usage);
    };
    reg.add(std::move(t));
}

}  // namespace judas
