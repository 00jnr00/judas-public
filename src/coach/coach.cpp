// Coach (P0): see coach.h and docs/COACH.md.
#include <map>
#include "coach/coach.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "skills/guard.h"
#include "skills/skills.h"

namespace judas {
namespace fs = std::filesystem;

namespace {

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

bool starts_with(const std::string & s, const std::string & p) { return s.rfind(p, 0) == 0; }

std::string read_file(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

bool write_file(const fs::path & p, const std::string & text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << text;
    return (bool) f;
}

// [a-z0-9][a-z0-9_-]{0,47}
bool valid_edit_name(const std::string & n) {
    if (n.empty() || n.size() > 48) return false;
    auto ok = [](char c, bool first) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (!first && (c == '-' || c == '_'));
    };
    for (size_t i = 0; i < n.size(); i++)
        if (!ok(n[i], i == 0)) return false;
    return true;
}

// Cites a step: "[12]", "[12-15]", "[3, 9]".
bool cites_step(const std::string & rationale) {
    static const std::regex re(R"(\[\s*\d+)");
    return std::regex_search(rationale, re);
}

std::string one_line(std::string s) {
    for (auto & c : s) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return trim(s);
}

std::string clip(const std::string & s, size_t cap) {
    if (s.size() <= cap) return s;
    size_t head = cap * 2 / 3, tail = cap - head;
    size_t h = head, t = s.size() - tail;
    return s.substr(0, h) + "\n[... " + std::to_string(s.size() - cap) + " chars cut ...]\n" + s.substr(t);
}

std::string stamp(long long ms) {
    std::time_t t = (std::time_t) (ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return b;
}

std::string iso(long long ms) {
    std::time_t t = (std::time_t) (ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}

constexpr size_t kSkillBodyCap = 4000;
constexpr size_t kOverlayBlockCap = 4000;

}  // namespace

// ---- safety floor ----------------------------------------------------------------------------------------

bool coach_forbidden(const std::string & text, std::string * why) {
    static const std::vector<std::pair<std::regex, const char *>> rules = [] {
        auto mk = [](const char * re, const char * label) {
            return std::make_pair(std::regex(re, std::regex::icase | std::regex::ECMAScript), label);
        };
        return std::vector<std::pair<std::regex, const char *>>{
            mk(R"(\b(sudo|doas|pkexec|passwd|faillock)\b)", "privilege escalation"),
            mk(R"(\bsu\s+(-|root\b))", "privilege escalation"),
            mk(R"(permission)", "permissions"),
            mk(R"(sandbox|bwrap|bubblewrap)", "sandbox"),
            mk(R"(safety[ _-]*floor)", "safety floor"),
            mk(R"(allow_paths|secret_paths|full-auto|\bbypass\w*)", "permission settings"),
            mk(R"(approv(al|er|e)\b)", "approvals"),
            mk(R"(config\.toml|secrets\.env)", "configuration files"),
            mk(R"((ignore|disregard|override|forget)\s+(all\s+|any\s+|the\s+)?(previous|prior|above|earlier|system)\s+(instructions|rules|prompt))",
               "instruction override"),
            mk(R"((disable|turn\s+off|skip|remove)\s+(the\s+)?(guard|safety|sandbox|checks?\b))", "disabling safeguards"),
        };
    }();
    for (auto & [re, label] : rules)
        if (std::regex_search(text, re)) {
            if (why) *why = label;
            return true;
        }
    return false;
}

std::string coach_check_edit(const Config & cfg, const CoachEdit & e) {
    if (e.kind != "overlay" && e.kind != "skill" && e.kind != "tool") return "unknown edit kind";
    if (trim(e.text).empty()) return "empty edit";
    if (trim(e.rationale).empty()) return "missing rationale";
    if (!cites_step(e.rationale)) return "rationale does not cite a transcript step like [12]";
    if (e.kind != "overlay" && !valid_edit_name(e.name)) return "invalid name '" + e.name + "'";
    std::string why;
    if (coach_forbidden(e.name + "\n" + e.text + "\n" + e.description, &why))
        return "touches the safety floor (" + why + ")";
    if (e.kind == "overlay" && (int) e.text.size() > cfg.coach.overlay_max_chars)
        return "overlay is over " + std::to_string(cfg.coach.overlay_max_chars) + " chars";
    if (e.kind == "tool" && (int) e.text.size() > cfg.coach.tools_max_chars)
        return "tool note is over " + std::to_string(cfg.coach.tools_max_chars) + " chars";
    if (e.kind == "skill") {
        if (e.text.size() > kSkillBodyCap) return "skill body is over " + std::to_string(kSkillBodyCap) + " chars";
        if (trim(e.description).empty()) return "skill has no description";
        if (scan_skill_text(e.text).blocked()) return "skill guard blocked the body";
    }
    return "";
}

// ---- parsing ---------------------------------------------------------------------------------------------

CoachProposal coach_parse(const std::string & reply, std::string * err) {
    CoachProposal out;
    std::istringstream in(reply);
    struct Block {
        std::string kind, name;
        std::vector<std::string> lines;
    };
    std::vector<Block> blocks;
    static const std::regex marker(R"(^\s*===\s*(OVERLAY|SKILL|TOOL|END)(?:\s+([A-Za-z0-9_-]+))?\s*===\s*$)",
                                   std::regex::icase);
    bool open = false;
    for (std::string line; std::getline(in, line);) {
        std::smatch m;
        if (std::regex_match(line, m, marker)) {
            std::string k = lower(m[1].str());
            if (k == "end") { open = false; continue; }
            blocks.push_back({k, lower(m[2].str()), {}});
            open = true;
            continue;
        }
        if (open) blocks.back().lines.push_back(line);
        else if (starts_with(lower(trim(line)), "summary:")) out.summary = one_line(trim(line).substr(8));
    }
    CoachEdit overlay;   // OVERLAY blocks merge into one edit
    overlay.kind = "overlay";
    for (auto & b : blocks) {
        std::string rationale, description, triggers;
        std::string body;
        for (auto & l : b.lines) {
            std::string t = lower(trim(l));
            if (body.empty() && starts_with(t, "rationale:")) rationale = one_line(trim(l).substr(10));
            else if (body.empty() && b.kind == "skill" && starts_with(t, "description:")) description = one_line(trim(l).substr(12));
            else if (body.empty() && b.kind == "skill" && starts_with(t, "triggers:")) triggers = trim(l).substr(9);
            else if (!body.empty() || !trim(l).empty()) body += l + "\n";
        }
        body = trim(body);
        if (b.kind == "overlay") {
            if (!rationale.empty()) overlay.rationale += (overlay.rationale.empty() ? "" : " ") + rationale;
            if (!body.empty()) overlay.text += (overlay.text.empty() ? "" : "\n") + body;
        } else {
            CoachEdit e;
            e.kind = b.kind;
            e.name = b.name;
            e.text = body;
            e.rationale = rationale;
            e.description = description;
            std::stringstream ts(triggers);
            for (std::string item; std::getline(ts, item, ',');) {
                item = lower(trim(item));
                if (item.size() >= 3 && item.find_first_of("[]\"'") == std::string::npos) e.triggers.push_back(item);
            }
            if (e.triggers.size() > 6) e.triggers.resize(6);
            out.edits.push_back(std::move(e));
        }
    }
    if (!trim(overlay.text).empty()) out.edits.insert(out.edits.begin(), overlay);
    if (err) {
        err->clear();
        if (out.summary.empty() && out.edits.empty())
            *err = "the model reply had no SUMMARY line and no edit blocks";
    }
    return out;
}

// ---- transcript ------------------------------------------------------------------------------------------

std::string coach_render_step(const MessageRow & m, int number, size_t output_cap, const std::string & label,
                              const std::vector<std::string> & secrets) {
    std::string head = "[" + std::to_string(number) + "] " + m.role;
    if (!label.empty()) head += " (" + label + ")";
    const json & b = m.body;
    std::string content;
    if (b.is_object() && b.contains("content")) {
        const json & c = b["content"];
        if (c.is_string()) content = c.get<std::string>();
        else if (c.is_array())
            for (auto & part : c)
                if (part.is_object() && part.value("type", "") == "text") content += part.value("text", "") + "\n";
    }
    std::string calls;
    if (b.is_object() && b.contains("tool_calls") && b["tool_calls"].is_array())
        for (auto & tc : b["tool_calls"]) {
            const json & f = tc.contains("function") ? tc["function"] : tc;
            std::string args = f.contains("arguments") && f["arguments"].is_string() ? f["arguments"].get<std::string>()
                                                                                       : f.value("arguments", json::object()).dump();
            calls += "\n  call " + f.value("name", "?") + "(" + clip(one_line(args), 240) + ")";
        }
    for (auto & s : secrets)
        if (!s.empty() && (content.find(s) != std::string::npos || calls.find(s) != std::string::npos))
            return head + ": [withheld: mentions a protected path]";
    size_t cap = m.role == "tool" ? output_cap : std::max<size_t>(output_cap * 3, 1200);
    return head + ": " + clip(trim(content), cap) + calls;
}

std::vector<std::string> coach_render_steps(const std::vector<MessageRow> & msgs, size_t output_cap,
                                            const std::map<std::string, std::string> & agent_roles,
                                            const std::vector<std::string> & secrets) {
    std::vector<std::string> out;
    int n = 0;
    for (auto & m : msgs) {
        if (m.role == "system") continue;
        std::string label;
        auto it = agent_roles.find(m.agent);
        if (it != agent_roles.end()) {
            if (it->second == "review") continue;
            if (it->second != "lead") label = it->second;
        }
        out.push_back(coach_render_step(m, ++n, output_cap, label, secrets));
    }
    return out;
}

std::string coach_trim_steps(const std::vector<std::string> & steps, size_t max_chars) {
    if (steps.empty()) return "";
    size_t total = 0;
    for (auto & s : steps) total += s.size() + 1;
    auto join = [&](size_t from, size_t to) {
        std::string o;
        for (size_t i = from; i < to; i++) o += steps[i] + "\n";
        return o;
    };
    if (total <= max_chars) return join(0, steps.size());
    // Always keep step one (the task); fill the rest from the end, where sessions usually go wrong.
    std::string first = steps[0].size() > max_chars / 4 ? clip(steps[0], max_chars / 4) + "\n" : steps[0] + "\n";
    std::string marker_room(64, ' ');
    size_t budget = max_chars > first.size() + marker_room.size() ? max_chars - first.size() - marker_room.size() : 0;
    size_t start = steps.size(), used = 0;
    while (start > 1 && used + steps[start - 1].size() + 1 <= budget) {
        used += steps[start - 1].size() + 1;
        start--;
    }
    std::string out = first;
    if (start > 1) out += "[... steps 2-" + std::to_string(start) + " omitted to fit the context ...]\n";
    out += join(start, steps.size());
    return out;
}

std::string coach_transcript(const Config & cfg, const Store & store, const std::string & session, size_t max_chars) {
    auto msgs = store.messages_page(session, 0, 0);
    std::map<std::string, std::string> roles;
    for (auto & a : store.list_agents(session)) roles[a.id] = a.parent.empty() ? "lead" : a.role;
    std::vector<std::string> secrets;
    for (auto & s : cfg.permissions.secret_paths) secrets.push_back(expand_home(s));
    // Shrink tool outputs first; only then drop the middle of the session.
    std::vector<std::string> steps;
    for (size_t cap : {size_t(700), size_t(300), size_t(120)}) {
        steps = coach_render_steps(msgs, cap, roles, secrets);
        size_t total = 0;
        for (auto & s : steps) total += s.size() + 1;
        if (total <= max_chars) break;
    }
    return coach_trim_steps(steps, max_chars);
}

// ---- prompts ---------------------------------------------------------------------------------------------

std::string coach_system_prompt(const Config & cfg) {
    std::ostringstream o;
    o << "You review one session of a local coding agent and suggest small changes to the agent's own guidance. "
         "Use only evidence in the transcript. Steps are numbered like [12].\n\n"
         "You may propose three kinds of edit:\n"
         "- OVERLAY: a few short rules added to the agent's system prompt (at most " << cfg.coach.overlay_max_chars
      << " characters in total).\n"
         "- SKILL: a reusable how-to in Markdown that the agent loads on demand (at most " << cfg.coach.max_skills
      << " skills).\n"
         "- TOOL: a short note on using one named tool better (all tool notes together at most " << cfg.coach.tools_max_chars
      << " characters).\n\n"
         "Rules:\n"
         "- Every edit needs a RATIONALE of one line that cites step numbers, for example [12] or [12-15], and says "
         "what went wrong or was wasted there.\n"
         "- Only propose what would have helped in several situations. Skip one-off facts, names, secrets and "
         "anything personal.\n"
         "- Never touch permissions, sandboxing, approvals, privilege escalation, config files or safety settings. "
         "Such edits are discarded.\n"
         "- Write general guidance. Never copy the task's own content (its rules, names, numbers or file contents) "
         "into an edit; the edit must help on a different task of the same kind.\n"
         "- Never suggest ways around harness limits, nudges or guards; fix the behavior that triggered them.\n"
         "- The SIGNALS block is measured by the harness, not by a model; trust it over the agent's own claims.\n"
         "- Fewer and sharper is better. If the session went well, write only the SUMMARY line.\n\n"
         "Reply in exactly this format and nothing else:\n"
         "SUMMARY: one or two sentences on how the session went\n"
         "=== OVERLAY ===\n"
         "RATIONALE: [steps] why\n"
         "- rule\n"
         "=== SKILL short-name ===\n"
         "DESCRIPTION: one line\n"
         "TRIGGERS: word, another word\n"
         "RATIONALE: [steps] why\n"
         "Markdown body\n"
         "=== TOOL tool_name ===\n"
         "RATIONALE: [steps] why\n"
         "the note\n"
         "=== END ===\n";
    return o.str();
}

std::string coach_signals(const Store & store, const std::string & sid) {
    std::ostringstream o;
    for (const auto & a : store.list_agents(sid)) {
        if (a.role == "review" || a.role == "reviewer_bg") continue;
        std::map<std::string, int> tools;
        int harness = 0, writes = 0, longest_read_streak = 0, streak = 0;
        for (const auto & m : store.messages(sid, a.id)) {
            if (m.role == "user") {
                std::string c = m.body.value("content", json("")).is_string() ? m.body.value("content", std::string()) : "";
                if (c.rfind("[harness]", 0) == 0) harness++;
            }
            if (m.role != "assistant" || !m.body.contains("tool_calls") || !m.body["tool_calls"].is_array()) continue;
            for (const auto & tc : m.body["tool_calls"]) {
                std::string n = tc.contains("function") ? tc["function"].value("name", std::string("?")) : "?";
                tools[n]++;
                bool reading = n == "read_file" || n == "list_dir" || n == "glob" || n == "grep";
                if (n == "write_file" || n == "edit_file") writes++;
                streak = reading ? streak + 1 : 0;
                longest_read_streak = std::max(longest_read_streak, streak);
            }
        }
        o << "- agent " << (a.parent.empty() ? "lead" : a.role) << ": status " << a.status << ", tokens in "
          << a.prompt_tokens << " out " << a.completion_tokens << ", writes " << writes
          << ", longest run of read-only calls " << longest_read_streak << ", harness nudges " << harness << ", tools:";
        for (auto & [n, c] : tools) o << " " << n << "=" << c;
        o << "\n";
    }
    return o.str();
}

CoachStats coach_stats(const Store & store, const std::string & sid) {
    CoachStats st;
    for (const auto & a : store.list_agents(sid)) {
        if (a.role == "review" || a.role == "reviewer_bg") continue;
        if (a.parent.empty() && st.status.empty()) st.status = a.status;
        st.tokens_in += a.prompt_tokens;
        st.tokens_out += a.completion_tokens;
        int streak = 0;
        for (const auto & m : store.messages(sid, a.id)) {
            if (m.role == "user") {
                std::string c = m.body.value("content", json("")).is_string() ? m.body.value("content", std::string()) : "";
                if (c.rfind("[harness]", 0) == 0) st.nudges++;
            }
            if (m.role != "assistant" || !m.body.contains("tool_calls") || !m.body["tool_calls"].is_array()) continue;
            for (const auto & tc : m.body["tool_calls"]) {
                std::string n = tc.contains("function") ? tc["function"].value("name", std::string("?")) : "?";
                st.tool_calls++;
                bool reading = n == "read_file" || n == "list_dir" || n == "glob" || n == "grep";
                if (n == "write_file" || n == "edit_file") st.writes++;
                streak = reading ? streak + 1 : 0;
                st.longest_read_streak = std::max(st.longest_read_streak, streak);
            }
        }
    }
    return st;
}

std::string coach_user_prompt(const SessionRow & s, const std::string & transcript, const std::string & signals) {
    std::ostringstream o;
    o << "Session: " << (s.title.empty() ? s.id : s.title) << "\nWorking directory: " << s.cwd << "\n\nSIGNALS:\n"
      << (signals.empty() ? "(none)\n" : signals) << "\nTranscript:\n\n" << transcript << "\n\nWrite your proposal now.";
    return o.str();
}

// ---- storage ---------------------------------------------------------------------------------------------

std::string coach_root(const Config & cfg) { return (fs::path(expand_home(cfg.data_dir)) / "coach").string(); }

std::string coach_overlay_path(const Config & cfg) {
    if (!cfg.coach.overlay.empty()) return expand_home(cfg.coach.overlay);
    return (fs::path(coach_root(cfg)) / "overlay.md").string();
}

bool coach_valid_id(const std::string & id) {
    if (id.empty() || id.size() > 160 || id[0] == '.') return false;
    for (char c : id)
        if (!(std::isalnum((unsigned char) c) || c == '-' || c == '_' || c == '.')) return false;
    return id.find("..") == std::string::npos;
}

namespace {

std::string skill_file(const CoachEdit & e, const std::string & id) {
    std::string t;
    for (size_t i = 0; i < e.triggers.size(); i++) t += (i ? ", " : "") + e.triggers[i];
    return "---\nname: " + e.name + "\ndescription: " + one_line(e.description) + "\ntriggers: [" + t +
           "]\nsource: coach\ncoach_entry: " + id + "\npinned: false\nversion: 1\n---\n\n" + trim(e.text) + "\n";
}

std::string readme(const CoachVariant & v, const std::string & status) {
    std::ostringstream o;
    o << "# Coach proposal " << v.id << "\n\nStatus: " << status << "\nSession: " << v.session << "\nModel: " << v.model
      << "\nCreated: " << iso(v.created_ms) << "\n\n## Summary\n\n" << (v.summary.empty() ? "(none)" : v.summary) << "\n\n## Edits\n\n";
    if (v.edits.empty()) o << "No edits proposed.\n\n";
    int n = 0;
    for (auto & e : v.edits) {
        o << "### " << ++n << ". " << e.kind << (e.name.empty() ? "" : " " + e.name) << "\n\nWhy: " << e.rationale << "\n\n";
        if (e.kind == "skill") o << "Description: " << e.description << "\n\n";
        o << "```\n" << trim(e.text) << "\n```\n\n";
    }
    if (!v.discarded.empty()) {
        o << "## Discarded\n\nThese edits were refused before writing:\n\n";
        for (auto & d : v.discarded) o << "- " << d.what << ": " << d.reason << "\n";
        o << "\n";
    }
    o << "## Files\n\n- overlay.md: rules for the agent prompt\n- tools.md: tool notes\n- skills/*.md: skills\n"
         "- meta.json: machine-readable copy of this proposal\n\nAdopt with `judas coach adopt " << v.id
      << "` or discard with `judas coach reject " << v.id << "`. Nothing here has any effect until it is adopted.\n";
    return o.str();
}

json edit_json(const CoachEdit & e) {
    return {{"kind", e.kind}, {"name", e.name}, {"rationale", e.rationale}, {"description", e.description},
            {"triggers", e.triggers}, {"text", e.text}};
}

void save_meta(const CoachVariant & v) {
    json j = {{"id", v.id}, {"session", v.session}, {"status", v.status}, {"model", v.model}, {"summary", v.summary},
              {"created_ms", v.created_ms}, {"edits", json::array()}, {"discarded", json::array()}};
    for (auto & e : v.edits) j["edits"].push_back(edit_json(e));
    for (auto & d : v.discarded) j["discarded"].push_back({{"what", d.what}, {"reason", d.reason}});
    if (v.probed) { j["probed"] = true; j["probe_verdict"] = v.probe_verdict; }
    write_file(fs::path(v.dir) / "meta.json", j.dump(2) + "\n");
    write_file(fs::path(v.dir) / "README.md", readme(v, v.status));
}

}  // namespace

CoachVariant coach_write_variant(const Config & cfg, const std::string & session, const CoachProposal & p,
                                 const std::string & model, long long now_ms) {
    CoachVariant v;
    v.session = session;
    v.model = model;
    v.summary = p.summary;
    v.created_ms = now_ms;
    v.status = "proposed";
    std::string base = stamp(now_ms) + "-" + session;
    for (auto & c : base)
        if (!(std::isalnum((unsigned char) c) || c == '-' || c == '_' || c == '.')) c = '_';
    fs::path root = coach_root(cfg);
    v.id = base;
    for (int i = 2; fs::exists(root / v.id); i++) v.id = base + "-" + std::to_string(i);
    v.dir = (root / v.id).string();

    int skills = 0;
    std::string overlay_text, tools_text;
    std::vector<std::string> seen_skill;
    for (auto e : p.edits) {
        std::string what = e.kind + (e.name.empty() ? "" : " " + e.name);
        std::string why = coach_check_edit(cfg, e);
        if (why.empty() && e.kind == "skill") {
            if (skills >= cfg.coach.max_skills) why = "more than coach.max_skills skills";
            else if (std::find(seen_skill.begin(), seen_skill.end(), e.name) != seen_skill.end()) why = "duplicate skill name";
        }
        if (why.empty() && e.kind == "tool" &&
            (int) (tools_text.size() + e.name.size() + e.text.size() + 8) > cfg.coach.tools_max_chars)
            why = "tool notes together are over " + std::to_string(cfg.coach.tools_max_chars) + " chars";
        if (why.empty() && e.kind == "overlay" && !overlay_text.empty()) why = "second overlay edit";
        if (!why.empty()) { v.discarded.push_back({what, why}); continue; }
        if (e.kind == "skill") {
            skills++;
            seen_skill.push_back(e.name);
            write_file(fs::path(v.dir) / "skills" / (e.name + ".md"), skill_file(e, v.id));
        } else if (e.kind == "overlay") {
            overlay_text = trim(e.text);
        } else {
            tools_text += "- `" + e.name + "`: " + one_line(e.text) + "\n";
        }
        v.edits.push_back(e);
    }
    std::error_code ec;
    fs::create_directories(v.dir, ec);
    write_file(fs::path(v.dir) / "overlay.md", overlay_text.empty() ? "" : overlay_text + "\n");
    write_file(fs::path(v.dir) / "tools.md", tools_text);
    save_meta(v);
    return v;
}

bool coach_load(const Config & cfg, const std::string & id, CoachVariant & out) {
    if (!coach_valid_id(id)) return false;
    fs::path dir = fs::path(coach_root(cfg)) / id;
    std::error_code ec;
    if (!fs::is_regular_file(dir / "meta.json", ec)) return false;
    json j = json::parse(read_file(dir / "meta.json"), nullptr, false);
    if (!j.is_object()) return false;
    CoachVariant v;
    v.id = id;
    v.dir = dir.string();
    v.session = j.value("session", "");
    v.status = j.value("status", "proposed");
    v.model = j.value("model", "");
    v.summary = j.value("summary", "");
    v.created_ms = j.value("created_ms", 0LL);
    v.probed = j.value("probed", false);
    v.probe_verdict = j.value("probe_verdict", "");
    if (j.contains("edits") && j["edits"].is_array())
        for (auto & e : j["edits"]) {
            CoachEdit ce;
            ce.kind = e.value("kind", "");
            ce.name = e.value("name", "");
            ce.rationale = e.value("rationale", "");
            ce.description = e.value("description", "");
            ce.text = e.value("text", "");
            if (e.contains("triggers") && e["triggers"].is_array())
                for (auto & t : e["triggers"]) if (t.is_string()) ce.triggers.push_back(t.get<std::string>());
            v.edits.push_back(std::move(ce));
        }
    if (j.contains("discarded") && j["discarded"].is_array())
        for (auto & d : j["discarded"]) v.discarded.push_back({d.value("what", ""), d.value("reason", "")});
    out = std::move(v);
    return true;
}

std::vector<CoachVariant> coach_list(const Config & cfg) {
    std::vector<CoachVariant> out;
    std::error_code ec;
    fs::path root = coach_root(cfg);
    if (!fs::is_directory(root, ec)) return out;
    for (auto & e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        CoachVariant v;
        if (coach_load(cfg, e.path().filename().string(), v)) out.push_back(std::move(v));
    }
    std::sort(out.begin(), out.end(), [](const CoachVariant & a, const CoachVariant & b) { return a.id > b.id; });
    return out;
}

CoachAdoptResult coach_adopt(const Config & cfg, const std::string & id) {
    CoachAdoptResult r;
    CoachVariant v;
    if (!coach_load(cfg, id, v)) { r.error = "no such proposal: " + id; return r; }
    if (v.status != "proposed") { r.error = "proposal is " + v.status + ", not proposed"; return r; }
    fs::path dir = v.dir;

    // The files are plain text a person may have edited: check what is on disk, not only the stored edits.
    std::string overlay = trim(read_file(dir / "overlay.md")), tools = trim(read_file(dir / "tools.md")), why;
    if (coach_forbidden(overlay + "\n" + tools, &why)) {
        r.error = "overlay or tool notes touch the safety floor (" + why + "); nothing adopted";
        return r;
    }
    if ((int) overlay.size() > cfg.coach.overlay_max_chars || (int) tools.size() > cfg.coach.tools_max_chars) {
        r.error = "overlay or tool notes are over the size caps; nothing adopted";
        return r;
    }
    struct Pending { std::string name, text; };
    std::vector<Pending> pending;
    std::error_code ec;
    if (fs::is_directory(dir / "skills", ec)) {
        std::vector<fs::path> files;
        for (auto & e : fs::directory_iterator(dir / "skills", ec))
            if (e.path().extension() == ".md") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (auto & f : files) {
            std::string name = f.stem().string(), text = read_file(f);
            if (!valid_edit_name(name)) { r.skipped.push_back(name + ": invalid name"); continue; }
            if (coach_forbidden(text, &why)) { r.skipped.push_back(name + ": touches the safety floor (" + why + ")"); continue; }
            if (scan_skill_text(text).blocked()) { r.skipped.push_back(name + ": skill guard blocked it"); continue; }
            pending.push_back({name, text});
        }
    }
    Skills skills(cfg.skills);
    fs::path sdir = skills.write_dir();
    if (!pending.empty() && sdir.empty()) {
        r.error = "no skills path configured";
        return r;
    }
    for (auto & p : pending) {
        fs::path dest = sdir / (p.name + ".md");
        if (fs::exists(dest, ec) || skills.by_name(p.name)) { r.skipped.push_back(p.name + ": a skill with that name exists"); continue; }
        if (!write_file(dest, p.text)) { r.skipped.push_back(p.name + ": could not write " + dest.string()); continue; }
        r.skills_added.push_back(p.name);
    }
    if (!overlay.empty() || !tools.empty()) {
        std::string block = "## coach " + v.id + "\n\n";
        if (!overlay.empty()) block += overlay + "\n\n";
        if (!tools.empty()) block += "Tool notes:\n" + tools + "\n\n";
        fs::path op = coach_overlay_path(cfg);
        std::string cur = fs::exists(op, ec) ? read_file(op) : "";
        if (!write_file(op, cur + (cur.empty() || cur.back() == '\n' ? "" : "\n") + block)) {
            r.error = "could not write " + op.string();
            return r;
        }
        r.overlay_appended = true;
    }
    v.status = "adopted";
    save_meta(v);
    r.ok = true;
    return r;
}

bool coach_reject(const Config & cfg, const std::string & id, std::string * err) {
    CoachVariant v;
    if (!coach_load(cfg, id, v)) { if (err) *err = "no such proposal: " + id; return false; }
    if (v.status != "proposed") { if (err) *err = "proposal is " + v.status + ", not proposed"; return false; }
    v.status = "rejected";
    save_meta(v);
    return true;
}

std::string coach_overlay_block(const Config & cfg) {
    if (!cfg.coach.enabled) return "";
    std::error_code ec;
    fs::path op = coach_overlay_path(cfg);
    if (!fs::is_regular_file(op, ec)) return "";
    std::string text = read_file(op);
    // Sections are "## coach <id>"; a section that matches the safety floor is dropped whole.
    std::vector<std::string> sections;
    std::istringstream in(text);
    std::string cur;
    for (std::string line; std::getline(in, line);) {
        if (starts_with(line, "## ") && !cur.empty()) { sections.push_back(cur); cur.clear(); }
        cur += line + "\n";
    }
    if (!trim(cur).empty()) sections.push_back(cur);
    std::vector<std::string> keep;
    for (auto & s : sections)
        if (!trim(s).empty() && !coach_forbidden(s)) keep.push_back(trim(s));
    // Newest sections win when the cap bites.
    std::string body;
    for (size_t i = keep.size(); i-- > 0;) {
        if (body.size() + keep[i].size() + 2 > kOverlayBlockCap) break;
        body = keep[i] + "\n\n" + body;
    }
    body = trim(body);
    if (body.empty()) return "";
    return "Adopted guidance (from reviewing earlier sessions; the user approved it):\n\n" + body;
}

json coach_variant_json(const CoachVariant & v, bool full) {
    json j = {{"id", v.id}, {"session", v.session}, {"status", v.status}, {"model", v.model}, {"summary", v.summary},
              {"created_ms", v.created_ms}, {"edit_count", v.edits.size()}, {"discarded_count", v.discarded.size()}, {"probed", v.probed}, {"probe_verdict", v.probe_verdict}};
    if (full) {
        j["edits"] = json::array();
        for (auto & e : v.edits) j["edits"].push_back(edit_json(e));
        j["discarded"] = json::array();
        for (auto & d : v.discarded) j["discarded"].push_back({{"what", d.what}, {"reason", d.reason}});
        j["dir"] = v.dir;
    }
    return j;
}

// ---- running ---------------------------------------------------------------------------------------------

CoachRunResult coach_propose(const Config & cfg, Engine & engine, Store & store, const std::string & session_arg,
                             const std::atomic<bool> * cancel) {
    CoachRunResult r;
    if (!cfg.coach.enabled) { r.error = "coach is disabled; set [coach] enabled = true in config.toml"; return r; }
    std::string sid = session_arg;
    if (sid.empty() || sid == "last") {
        auto l = store.list_sessions(1);
        if (l.empty()) { r.error = "no sessions yet"; return r; }
        sid = l[0].id;
    }
    auto sess = store.get_session(sid);
    if (!sess) { r.error = "no such session: " + sid; return r; }

    if (store.last_message_id(sid) == 0) { r.error = "session has no messages"; return r; }

    // Budget: the whole prompt must leave room for the reply. Shrink the transcript until it fits.
    EngineState st = engine.state();
    int ctx = st.ctx_total > 0 ? st.ctx_total : 24576;
    int max_prompt = std::max(2000, std::min((int) (ctx * 0.6), ctx - 6000));
    std::string sys = coach_system_prompt(cfg);
    size_t chars = (size_t) max_prompt * 3;
    std::string user, sig = coach_signals(store, sid);
    for (int i = 0; i < 8; i++) {
        user = coach_user_prompt(*sess, coach_transcript(cfg, store, sid, chars), sig);
        if (engine.count_tokens(sys) + engine.count_tokens(user) <= max_prompt) break;
        chars = chars * 8 / 10;
    }

    ChatRequest req;
    req.messages = {{"system", sys}, {"user", user}};
    req.enable_thinking = false;
    req.sampling.temp = 0.3f;
    req.sampling.max_tokens = 2500;
    req.priority = 2;   // background: yields to anything the user is doing
    req.tag = "coach " + sid;
    ChatResult res = engine.chat(req, {}, cancel);
    if (cancel && cancel->load()) { r.error = "cancelled"; return r; }
    if (!res.error.empty() || res.finish_reason == "error") { r.error = res.error.empty() ? "model error" : res.error; return r; }
    std::string perr;
    CoachProposal p = coach_parse(res.message.content, &perr);
    if (!perr.empty()) { r.error = perr; return r; }
    r.variant = coach_write_variant(cfg, sid, p, engine.model_name(), now_ms());
    r.ok = true;
    return r;
}

}  // namespace judas

namespace judas {

bool coach_set_probe(const Config & cfg, const std::string & id, const std::string & verdict) {
    CoachVariant v;
    if (!coach_load(cfg, id, v)) return false;
    v.probed = true;
    v.probe_verdict = verdict;
    save_meta(v);
    return true;
}

}  // namespace judas
