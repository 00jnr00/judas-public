// Learn mode helpers and tools. See tutor.h.
#include "tutor/tutor.h"

#include "tutor/learn_store.h"

#include <fnmatch.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>

namespace judas {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string slurp(const std::string & path, size_t max) {
    std::ifstream f(path, std::ios::binary);
    std::string out(max, '\0');
    f.read(out.data(), (std::streamsize) max);
    out.resize((size_t) f.gcount());
    return out;
}

std::string cut_utf8(const std::string & s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

bool has_glob(const std::string & s) { return s.find_first_of("*?[") != std::string::npos; }

// Expand one absolute pattern segment by segment.
void expand_pattern(const fs::path & base, std::vector<std::string> segs, size_t i, std::vector<fs::path> & out) {
    std::error_code ec;
    if (i == segs.size()) {
        out.push_back(base);
        return;
    }
    if (!has_glob(segs[i])) {
        expand_pattern(base / segs[i], segs, i + 1, out);
        return;
    }
    for (auto & e : fs::directory_iterator(base, ec)) {
        std::string name = e.path().filename().string();
        if (fnmatch(segs[i].c_str(), name.c_str(), FNM_PERIOD) == 0) expand_pattern(e.path(), segs, i + 1, out);
    }
}

bool is_text_name(const fs::path & p) {
    std::string ext = lower(p.extension().string());
    return ext == ".md" || ext == ".txt" || ext == ".json" || ext == ".toml" || ext == ".yaml" || ext == ".yml" || ext.empty();
}

std::string canon(const std::string & p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(p, ec);
    return ec ? p : c.string();
}

}  // namespace

std::vector<std::string> learn_context_paths(const Config & cfg) {
    std::set<std::string> seen;
    std::vector<std::string> out;
    std::error_code ec;
    auto add = [&](const fs::path & p) {
        if (!fs::is_regular_file(p, ec)) return;
        std::string c = canon(p.string());
        if (seen.insert(c).second) out.push_back(c);
    };
    for (const std::string & raw : cfg.learn.context_files) {
        std::string pat = expand_home(raw);
        if (pat.empty() || pat[0] != '/') continue;   // only absolute (or ~) patterns
        std::vector<std::string> segs;
        std::stringstream ss(pat);
        std::string seg;
        while (std::getline(ss, seg, '/'))
            if (!seg.empty()) segs.push_back(seg);
        std::vector<fs::path> hits;
        expand_pattern("/", segs, 0, hits);
        for (auto & h : hits) {
            if (fs::is_directory(h, ec)) {
                for (auto & e : fs::directory_iterator(h, ec))
                    if (is_text_name(e.path())) add(e.path());
            } else {
                add(h);
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool learn_is_context_file(const Config & cfg, const std::string & path) {
    if (cfg.learn.context_files.empty()) return false;
    std::string c = canon(path);
    for (auto & p : learn_context_paths(cfg))
        if (p == c) return true;
    return false;
}

std::string learn_context_digest(const Config & cfg) {
    auto files = learn_context_paths(cfg);
    size_t total = (size_t) std::max(0, cfg.learn.max_context_tokens) * 4;
    if (files.empty() || total == 0) return "";
    size_t per = std::max<size_t>(200, total / files.size());
    std::string out;
    for (auto & f : files) {
        std::string body = slurp(f, per + 4);
        bool cut = body.size() > per;
        body = cut_utf8(body, per);
        out += "### " + fs::path(f).filename().string() + "\n" + body;
        if (!body.empty() && body.back() != '\n') out += "\n";
        if (cut) out += "[cut; learner_context reads the rest]\n";
        out += "\n";
    }
    return out;
}

std::vector<std::string> learn_context_search(const Config & cfg, const std::string & query, size_t max) {
    std::vector<std::string> hits;
    std::string q = lower(query);
    if (q.empty()) return hits;
    for (auto & f : learn_context_paths(cfg)) {
        std::ifstream in(f);
        std::string line;
        int n = 0;
        while (std::getline(in, line) && hits.size() < max) {
            n++;
            if (lower(line).find(q) != std::string::npos)
                hits.push_back(fs::path(f).filename().string() + ":" + std::to_string(n) + ": " + cut_utf8(line, 300));
        }
        if (hits.size() >= max) break;
    }
    return hits;
}

std::string learn_context_read(const Config & cfg, const std::string & which, size_t max_bytes, std::string * err) {
    auto files = learn_context_paths(cfg);
    std::string want = canon(expand_home(which));
    for (auto & f : files)
        if (f == want || fs::path(f).filename() == which) {
            std::string t = slurp(f, max_bytes + 4);
            bool cut = t.size() > max_bytes;
            t = cut_utf8(t, max_bytes);
            if (cut) t += "\n[cut at " + std::to_string(max_bytes) + " bytes]";
            return t;
        }
    if (err) {
        *err = "not a learner context file: " + which + ". Available:";
        for (auto & f : files) *err += " " + fs::path(f).filename().string();
    }
    return "";
}

std::string learn_progress_path(const Config & cfg) {
    return (fs::path(expand_home(cfg.data_dir)) / "learn" / "progress.json").string();
}

std::string learn_ask_check(const json & a, json * norm) {
    if (!a.is_object()) return "arguments must be an object";
    static const std::set<std::string> kinds = {"predict", "check", "probe", "practice"};
    std::string kind = a.value("kind", "");
    if (!kinds.count(kind)) return "kind must be predict, check, probe or practice";
    if (!a.contains("prompt") || !a["prompt"].is_string()) return "prompt is required (markdown text)";
    std::string prompt = a["prompt"].get<std::string>();
    if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) return "prompt is empty";
    if (prompt.size() > 4000) return "prompt is too long (4000 characters at most); ask one thing";
    json opts = json::array();
    if (a.contains("options") && !a["options"].is_null()) {
        if (!a["options"].is_array()) return "options must be an array of strings";
        for (auto & o : a["options"]) {
            if (!o.is_string() || o.get<std::string>().empty()) return "every option must be a non-empty string";
            if (o.get<std::string>().size() > 200) return "an option is too long (200 characters at most)";
            opts.push_back(o);
        }
        if (opts.size() == 1) return "give at least two options, or none for a free answer";
        if (opts.size() > 8) return "at most 8 options";
    }
    for (const char * k : {"want_confidence", "want_thinking"})
        if (a.contains(k) && !a[k].is_boolean() && !a[k].is_null()) return std::string(k) + " must be true or false";
    if (norm) {
        *norm = {{"kind", kind}, {"prompt", prompt}, {"options", opts},
                 {"want_confidence", a.value("want_confidence", false)}, {"want_thinking", a.value("want_thinking", false)}};
    }
    return "";
}

bool learn_ends_turn(const std::string & tool) { return tool == "learn_ask"; }

const char * learn_prompt() {
    return R"(Mode: Learn. You are a tutor. The aim is understanding the learner can rebuild alone. They are probably on a phone: keep every turn short.

Rules you always follow:
1. Every question to the learner goes through learn_ask. Never end a message with a question in plain text. Write the teaching text first, call learn_ask last, then stop.
2. Call learn_plan whenever the phase or the plan changes: when you start, when you move to the next phase, when you write or change the steps, when a step becomes now, done or repair. Use one topic name per subject.
3. One idea per turn, one question per turn.

Phases, in order:
- measure: find what they already hold. Check the learner context below first (learner_context searches it) and never ask for what is written there. Ask 2 or 3 learn_ask questions, kind probe or predict, with want_confidence true.
- plan: call learn_plan with phase plan and 3 to 7 steps (id, title, depends_on, status todo). Skip what they know. Then set phase teach and the first step to now.
- teach: explain the step in a few sentences with one concrete example, then learn_ask kind check. A wrong or unsure answer: set that step to repair with learn_plan update, explain it another way, ask again. A good answer: mark it done and move on.
- practice: learn_ask kind practice with a new problem that needs the idea but does not name it. Harder only after a success.

Other tools:
- learn_note: when the learner shows they can use an idea, save 1 to 3 short propositions. In premises, give an earlier proposition by its id (such as "p2") or plain words for what it rests on.
- Facts about the learner that matter in later sessions go to memory_append. Use web_search before teaching a fact you doubt. Never write to the learner's files.)";
}

bool learn_tool_allowed(const std::string & n) {
    static const std::set<std::string> ok = {
        "learner_context", "learn_ask", "learn_plan", "learn_note", "memory_read", "memory_search", "memory_append", "read_file", "list_dir",
        "glob", "grep", "web_search", "web_fetch", "load_skill", "recall", "update_plan", "done", "session_search",
        "view_image", "spawn_agent", "wait_agents", "send_agent", "list_agents", "notify"};
    return ok.count(n) > 0;
}

namespace {

std::string today_str() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

ToolResult t_context(const json & a, ToolContext & ctx) {
    static const Config def;
    const Config & cfg = ctx.cfg ? *ctx.cfg : def;
    std::string file = a.value("file", ""), q = a.value("query", "");
    if (!file.empty()) {
        std::string err;
        std::string t = learn_context_read(cfg, file, 20000, &err);
        if (t.empty() && !err.empty()) return {false, err};
        return {true, t};
    }
    if (!q.empty()) {
        auto hits = learn_context_search(cfg, q);
        if (hits.empty()) return {true, "no matches for '" + q + "'"};
        std::string o;
        for (auto & h : hits) o += h + "\n";
        return {true, o};
    }
    auto files = learn_context_paths(cfg);
    if (files.empty()) return {true, "no learner context files are configured ([learn] context_files)"};
    std::string o = "Learner context files (read only):\n";
    for (auto & f : files) o += "- " + fs::path(f).filename().string() + " (" + f + ")\n";
    return {true, o};
}

ToolResult t_ask(const json & a, ToolContext &) {
    std::string err = learn_ask_check(a);
    if (!err.empty()) return {false, err};
    return {true, "The question is on the learner's screen. Your turn ends here; their answer arrives as the next message."};
}

// Shared by learn_plan and learn_note: load, apply under the lock, save on success.
ToolResult topics_call(const json & a, ToolContext & ctx,
                       std::string (*apply)(json &, const json &, bool *, const std::string &, const std::string &)) {
    static const Config def;
    const Config & cfg = ctx.cfg ? *ctx.cfg : def;
    std::lock_guard lk(learn_topics_mutex());
    json doc = learn_topics_load_migrated(cfg);
    bool ok = false;
    std::string reply = apply(doc, a, &ok, today_str(), ctx.session);
    if (!ok) return {false, reply};
    std::string err;
    if (!learn_topics_save(cfg, doc, &err)) return {false, err};
    return {true, reply};
}
ToolResult t_plan(const json & a, ToolContext & ctx) { return topics_call(a, ctx, learn_plan_apply); }
ToolResult t_note(const json & a, ToolContext & ctx) { return topics_call(a, ctx, learn_note_apply); }

}  // namespace

void register_learn_tools(ToolRegistry & reg) {
    auto str = [](const char * d) { return json{{"type", "string"}, {"description", d}}; };
    Tool c;
    c.name = "learner_context";
    c.description = "Read-only access to the learner's own files (goals, notes, plans). No arguments lists the files; "
                    "query searches all of them; file returns one in full. These files can never be changed.";
    c.parameters = {{"type", "object"},
                    {"properties", {{"query", str("Text to find, case-insensitive")}, {"file", str("A listed file name")}}},
                    {"required", json::array()}};
    c.run = t_context;
    reg.add(std::move(c));
    Tool q;
    q.name = "learn_ask";
    q.description = "Put one question in front of the learner as a card and end your turn. kind: predict (what will happen "
                    "before you show it), check (did the last idea land), probe (find where understanding stops), practice "
                    "(a fresh problem). prompt is markdown. options gives answer buttons; leave it out for a free answer box. "
                    "want_confidence adds a 4 step confidence control, want_thinking a box for the learner's reasoning. "
                    "Their reply comes back as the next message.";
    q.parameters = {{"type", "object"},
                    {"properties",
                     {{"kind", {{"type", "string"}, {"enum", {"predict", "check", "probe", "practice"}}}},
                      {"prompt", str("The question, markdown")},
                      {"options", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "2 to 8 answer buttons; omit for a free answer"}}},
                      {"want_confidence", {{"type", "boolean"}}},
                      {"want_thinking", {{"type", "boolean"}}}}},
                    {"required", {"kind", "prompt"}}};
    q.run = t_ask;
    reg.add(std::move(q));
    Tool pl;
    pl.name = "learn_plan";
    pl.description = "Keep the learner's plan for a topic where they can see it. phase is where the session is: measure "
                     "(finding what they hold), plan, teach or practice. steps replaces the whole plan: each has id, "
                     "title, depends_on (ids of steps that must come first) and status todo, now, done or repair. "
                     "update changes only the status of listed steps. Send phase alone to move on.";
    pl.parameters = {{"type", "object"},
                     {"properties",
                      {{"topic", str("Short topic name; reuse it across calls")},
                       {"phase", {{"type", "string"}, {"enum", {"measure", "plan", "teach", "practice"}}}},
                       {"steps",
                        {{"type", "array"},
                         {"items",
                          {{"type", "object"},
                           {"properties",
                            {{"id", str("Short id such as s1")},
                             {"title", str("What this step teaches")},
                             {"depends_on", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                             {"status", {{"type", "string"}, {"enum", {"todo", "now", "done", "repair"}}}}}},
                           {"required", {"id", "title"}}}}}},
                       {"update",
                        {{"type", "array"},
                         {"items",
                          {{"type", "object"},
                           {"properties",
                            {{"id", str("Step id")}, {"status", {{"type", "string"}, {"enum", {"todo", "now", "done", "repair"}}}}}},
                           {"required", {"id", "status"}}}}}}}},
                     {"required", {"topic"}}};
    pl.run = t_plan;
    reg.add(std::move(pl));
    Tool nt;
    nt.name = "learn_note";
    nt.description = "Write down what the learner now understands as 1 to 3 short propositions, each with the premises "
                     "it rests on (earlier propositions by id such as p2, or plain statements). Later Learn sessions on "
                     "the topic start with them. Only note what the learner showed they can use.";
    nt.parameters = {{"type", "object"},
                     {"properties",
                      {{"topic", str("Same topic name as learn_plan")},
                       {"notes",
                        {{"type", "array"},
                         {"items",
                          {{"type", "object"},
                           {"properties",
                            {{"text", str("One proposition, a sentence")},
                             {"premises", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                           {"required", {"text"}}}}}}}},
                     {"required", {"topic", "notes"}}};
    nt.run = t_note;
    reg.add(std::move(nt));
}

}  // namespace judas
