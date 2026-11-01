// Learn mode helpers and tools. See tutor.h.
#include "tutor/tutor.h"

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

namespace {

json str_list(const json & v, size_t max_items = 40) {
    json out = json::array();
    if (v.is_array())
        for (auto & x : v)
            if (x.is_string() && out.size() < max_items) out.push_back(cut_utf8(x.get<std::string>(), 300));
    return out;
}

std::string summarize(const std::string & topic, const json & t) {
    std::ostringstream o;
    o << "Topic: " << topic << "\n";
    if (t.contains("goal")) o << "Goal: " << t["goal"].get<std::string>() << "\n";
    if (t.contains("held")) {
        o << "Held:";
        for (auto & x : t["held"]) o << " [" << x.get<std::string>() << "]";
        o << "\n";
    }
    if (t.contains("gaps")) {
        o << "Gaps:";
        for (auto & x : t["gaps"]) o << " [" << x.get<std::string>() << "]";
        o << "\n";
    }
    if (t.contains("plan")) {
        int i = 0;
        for (auto & s : t["plan"]) {
            o << ++i << ". [" << (s.value("done", false) ? "x" : " ") << "] " << s.value("step", "");
            if (!s.value("note", "").empty()) o << " (" << s.value("note", "") << ")";
            o << "\n";
        }
    }
    if (t.contains("practice")) {
        size_t n = t["practice"].size(), from = n > 5 ? n - 5 : 0;
        for (size_t i = from; i < n; i++)
            o << "Practice " << t["practice"][i].value("date", "") << ": " << t["practice"][i].value("problem", "") << " -> "
              << t["practice"][i].value("result", "") << "\n";
    }
    return o.str();
}

}  // namespace

std::string learn_progress_apply(json & doc, const json & args, bool * ok, const std::string & today) {
    *ok = true;
    if (!doc.is_object()) doc = json::object();
    if (!doc.contains("topics") || !doc["topics"].is_object()) doc["topics"] = json::object();
    json & topics = doc["topics"];
    std::string action = args.value("action", "show");
    std::string topic = args.value("topic", "");
    if (action == "show") {
        if (topic.empty()) {
            if (topics.empty()) return "no learning topics recorded yet";
            std::string o = "Topics:\n";
            for (auto it = topics.begin(); it != topics.end(); ++it)
                o += "- " + it.key() + " (" + it.value().value("updated", "") + ")\n";
            return o;
        }
        if (!topics.contains(topic)) { *ok = false; return "no topic '" + topic + "'"; }
        return summarize(topic, topics[topic]);
    }
    if (topic.empty()) { *ok = false; return "topic is required for " + action; }
    if (topics.size() >= 200 && !topics.contains(topic)) { *ok = false; return "too many topics (200)"; }
    json & t = topics[topic];
    if (!t.is_object()) t = json::object();
    t["updated"] = today;
    if (action == "set_goal") {
        std::string g = args.value("goal", "");
        if (g.empty()) { *ok = false; return "goal is empty"; }
        t["goal"] = cut_utf8(g, 600);
    } else if (action == "probe") {
        t["held"] = str_list(args.value("held", json::array()));
        t["gaps"] = str_list(args.value("gaps", json::array()));
    } else if (action == "plan") {
        json steps = str_list(args.value("steps", json::array()), 60);
        if (steps.empty()) { *ok = false; return "steps is empty"; }
        json plan = json::array();
        for (auto & s : steps) plan.push_back({{"step", s}, {"done", false}});
        t["plan"] = plan;
    } else if (action == "step_done") {
        if (!t.contains("plan")) { *ok = false; return "topic has no plan"; }
        int idx = args.value("index", 0);
        if (idx < 1 || idx > (int) t["plan"].size()) { *ok = false; return "index must be in 1.." + std::to_string(t["plan"].size()); }
        t["plan"][idx - 1]["done"] = true;
        std::string note = args.value("note", "");
        if (!note.empty()) t["plan"][idx - 1]["note"] = cut_utf8(note, 300);
    } else if (action == "practice") {
        std::string pr = args.value("problem", ""), res = args.value("result", "");
        if (pr.empty() || res.empty()) { *ok = false; return "practice needs problem and result"; }
        if (!t.contains("practice")) t["practice"] = json::array();
        t["practice"].push_back({{"date", today}, {"problem", cut_utf8(pr, 300)}, {"result", cut_utf8(res, 300)}});
        while (t["practice"].size() > 50) t["practice"].erase(t["practice"].begin());
    } else {
        *ok = false;
        return "unknown action '" + action + "' (show, set_goal, probe, plan, step_done, practice)";
    }
    return "saved\n" + summarize(topic, t);
}

const char * learn_prompt() {
    return R"(Mode: Learn. You are a tutor. The aim is understanding the learner can rebuild on their own, not facts they can recite.

How a session goes:
1. Orient. Find what the learner wants and why. Their goals, notes and plans are in the learner context below and reachable with learner_context; use them so you never ask for what they already wrote down. Check learn_progress for earlier work on the topic and pick up from it. Long-term facts about the learner are in memory (memory_search); save a durable fact there only when it will matter in later sessions.
2. Probe. Ask a few short questions, one at a time, to find where their understanding stops. Ask for their reasoning, not only an answer. Record what they hold and what is missing with learn_progress (probe).
3. Plan. Name the shortest chain of ideas from what they hold to the goal. Leave out what they already know and what does not lead to the goal. Show the plan in a few lines and store it with learn_progress (plan).
4. Teach in small steps. Each step rests on something they already hold, adds one idea, and ends with one question that tests it. If the answer shows a gap, repair it before moving on. Mark steps done with learn_progress (step_done).
5. Practice. Later, set problems that need the idea without naming it, and record the result (practice). Raise difficulty only when they succeed.

Style: plain prose, short turns, one question at a time. Do not narrate your method. Say when you are unsure; use web_search to check facts you are not certain of before teaching them. Never write to the learner's context files; they belong to them. Keep each reply focused on the next step.)";
}

bool learn_tool_allowed(const std::string & n) {
    static const std::set<std::string> ok = {
        "learner_context", "learn_progress", "memory_read", "memory_search", "memory_append", "read_file", "list_dir",
        "glob", "grep", "web_search", "web_fetch", "load_skill", "recall", "update_plan", "done", "session_search",
        "view_image", "spawn_agent", "wait_agents", "send_agent", "list_agents", "notify"};
    return ok.count(n) > 0;
}

namespace {

std::mutex g_progress_mu;

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

ToolResult t_progress(const json & a, ToolContext & ctx) {
    static const Config def;
    const Config & cfg = ctx.cfg ? *ctx.cfg : def;
    std::lock_guard lk(g_progress_mu);
    std::string path = learn_progress_path(cfg);
    json doc = json::object();
    {
        std::ifstream in(path);
        if (in) doc = json::parse(in, nullptr, false);
        if (doc.is_discarded()) doc = json::object();
    }
    bool ok = true;
    std::string reply = learn_progress_apply(doc, a, &ok, today_str());
    if (ok && a.value("action", "show") != "show") {
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << doc.dump(1) << "\n";
            if (!out) return {false, "cannot write " + tmp};
        }
        fs::rename(tmp, path, ec);
        if (ec) return {false, "cannot write " + path};
    }
    return {ok, reply};
}

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
    Tool p;
    p.name = "learn_progress";
    p.description = "The learner's progress record, kept per topic. Actions: show (all topics, or one with topic), "
                    "set_goal(topic, goal), probe(topic, held[], gaps[]), plan(topic, steps[]), "
                    "step_done(topic, index, note), practice(topic, problem, result).";
    p.parameters = {{"type", "object"},
                    {"properties",
                     {{"action", str("show | set_goal | probe | plan | step_done | practice")},
                      {"topic", str("Topic name")},
                      {"goal", str("What the learner wants to understand")},
                      {"held", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"gaps", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"steps", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                      {"index", {{"type", "integer"}, {"description", "1-based plan step"}}},
                      {"note", str("Short note")},
                      {"problem", str("The practice problem")},
                      {"result", str("How it went")}}},
                    {"required", {"action"}}};
    p.run = t_progress;
    reg.add(std::move(p));
}

}  // namespace judas
