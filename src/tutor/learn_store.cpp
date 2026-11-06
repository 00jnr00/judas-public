// Learn v2 storage and tool logic. See learn_store.h.
#include "tutor/learn_store.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace judas {
namespace fs = std::filesystem;

namespace {

const char * kPhases[] = {"measure", "plan", "teach", "practice"};
const char * kStatuses[] = {"todo", "now", "done", "repair"};

bool in_list(const std::string & v, const char * const * list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (v == list[i]) return true;
    return false;
}
bool valid_phase(const std::string & v) { return in_list(v, kPhases, 4); }
bool valid_status(const std::string & v) { return in_list(v, kStatuses, 4); }

std::string cut(const std::string & s, size_t n) {
    if (s.size() <= n) return s;
    while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}
std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}
bool valid_step_id(const std::string & id) {
    if (id.empty() || id.size() > 24) return false;
    for (unsigned char c : id)
        if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
    return true;
}

// Finds or creates the topic; the display name is kept as given.
json & topic_for(json & doc, const std::string & name, const std::string & today, const std::string & session, std::string * id_out) {
    if (!doc.is_object()) doc = json::object();
    if (!doc.contains("topics") || !doc["topics"].is_object()) doc["topics"] = json::object();
    std::string id = learn_slug(name);
    json & t = doc["topics"][id];
    if (!t.is_object()) {
        t = {{"name", cut(trim(name), 80)}, {"phase", "measure"}, {"steps", json::array()}, {"props", json::array()},
             {"next_prop", 1}, {"created", today}};
    }
    t["updated"] = today;
    if (!session.empty()) t["session"] = session;
    if (id_out) *id_out = id;
    return t;
}

std::string render_plan(const std::string & id, const json & t) {
    std::ostringstream o;
    o << "Topic: " << t.value("name", id) << " (" << id << "), phase: " << t.value("phase", "measure") << "\n";
    for (auto & s : t.value("steps", json::array())) {
        o << "- [" << s.value("status", "todo") << "] " << s.value("id", "") << ": " << s.value("title", "");
        if (s.contains("depends_on") && !s["depends_on"].empty()) {
            o << " (after";
            for (auto & d : s["depends_on"]) o << " " << d.get<std::string>();
            o << ")";
        }
        o << "\n";
    }
    return o.str();
}

bool same_claim(const std::string & a, const std::string & b);   // defined after learn_norm

bool is_short_id(const std::string & s) {
    if (s.size() < 2 || s[0] != 'p') return false;
    for (size_t i = 1; i < s.size(); i++)
        if (!std::isdigit((unsigned char) s[i])) return false;
    return true;
}

}  // namespace

std::string learn_norm(const std::string & text) {
    std::string out;
    bool gap = false;
    for (unsigned char c : text) {
        if (c < 128 && std::isalnum(c)) {
            if (gap && !out.empty()) out += ' ';
            gap = false;
            out += (char) std::tolower(c);
        } else {
            gap = true;
        }
    }
    return out;
}

namespace {
// Equal under learn_norm; text with no ASCII letters or digits (normalizes to "") compares by its trimmed lowercase form.
bool same_claim(const std::string & a, const std::string & b) {
    std::string na = learn_norm(a), nb = learn_norm(b);
    if (na.empty() || nb.empty()) return na.empty() && nb.empty() && lower(trim(a)) == lower(trim(b));
    return na == nb;
}
}  // namespace

std::string learn_premise_ref(const json & props, const std::string & premise) {
    if (!props.is_array()) return "";
    std::string p = trim(premise);
    if (!p.empty() && p[0] == '#') p = trim(p.substr(1));
    std::string id = lower(p);
    if (is_short_id(id))
        for (auto & e : props)
            if (e.value("id", "") == id) return id;
    std::string n = learn_norm(premise);
    if (n.empty()) return "";
    for (auto & e : props)
        if (learn_norm(e.value("text", "")) == n) return e.value("id", "");
    return "";
}

std::string learn_slug(const std::string & name) {
    std::string out;
    bool dash = false;
    for (unsigned char c : name) {
        if (std::isalnum(c) && c < 128) {
            if (dash && !out.empty()) out += '-';
            dash = false;
            out += (char) std::tolower(c);
        } else {
            dash = true;
        }
    }
    if (out.size() > 48) out.resize(48);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::string learn_topics_path(const Config & cfg) {
    return (fs::path(expand_home(cfg.data_dir)) / "learn" / "topics.json").string();
}

std::mutex & learn_topics_mutex() {
    static std::mutex m;
    return m;
}

json learn_topics_load(const Config & cfg) {
    json doc = json::object();
    std::ifstream in(learn_topics_path(cfg));
    if (in) doc = json::parse(in, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) doc = json::object();
    if (!doc.contains("topics") || !doc["topics"].is_object()) doc["topics"] = json::object();
    return doc;
}

bool learn_topics_save(const Config & cfg, const json & doc, std::string * err) {
    std::string path = learn_topics_path(cfg);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << doc.dump(1) << "\n";
        if (!out) {
            if (err) *err = "cannot write " + tmp;
            return false;
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        if (err) *err = "cannot write " + path;
        return false;
    }
    return true;
}

std::string learn_plan_apply(json & doc, const json & args, bool * ok, const std::string & today, const std::string & session) {
    *ok = false;
    if (!args.is_object()) return "arguments must be an object";
    std::string name = trim(args.value("topic", ""));
    if (name.empty() || learn_slug(name).empty()) return "topic is required (a short name such as \"hash tables\")";
    if (name.size() > 80) return "topic name is too long (80 characters at most)";
    bool has_phase = args.contains("phase") && !args["phase"].is_null();
    bool has_steps = args.contains("steps") && !args["steps"].is_null();
    bool has_update = args.contains("update") && !args["update"].is_null();
    if (!has_phase && !has_steps && !has_update) return "give phase, steps or update";
    std::string phase;
    if (has_phase) {
        if (!args["phase"].is_string() || !valid_phase(args["phase"].get<std::string>()))
            return "phase must be measure, plan, teach or practice";
        phase = args["phase"].get<std::string>();
    }
    // validate everything before touching the document
    json steps;
    if (has_steps) {
        if (!args["steps"].is_array() || args["steps"].empty()) return "steps must be a non-empty array";
        if (args["steps"].size() > 30) return "at most 30 steps";
        steps = json::array();
        std::set<std::string> ids;
        for (auto & s : args["steps"]) {
            if (!s.is_object()) return "every step must be an object {id, title, depends_on?, status?}";
            std::string id = s.value("id", ""), title = trim(s.value("title", ""));
            if (!valid_step_id(id)) return "step id must be 1 to 24 letters, digits, - or _";
            if (!ids.insert(id).second) return "duplicate step id '" + id + "'";
            if (title.empty()) return "step '" + id + "' has no title";
            std::string st = s.contains("status") && !s["status"].is_null() ? s.value("status", "") : "todo";
            if (!valid_status(st)) return "status must be todo, now, done or repair";
            json deps = json::array();
            if (s.contains("depends_on") && !s["depends_on"].is_null()) {
                if (!s["depends_on"].is_array()) return "depends_on must be an array of step ids";
                for (auto & d : s["depends_on"]) {
                    if (!d.is_string()) return "depends_on holds step ids (strings)";
                    deps.push_back(d);
                }
            }
            steps.push_back({{"id", id}, {"title", cut(title, 120)}, {"depends_on", deps}, {"status", st}});
        }
        std::map<std::string, std::vector<std::string>> g;
        for (auto & s : steps)
            for (auto & d : s["depends_on"]) {
                std::string dep = d.get<std::string>(), id = s["id"].get<std::string>();
                if (!ids.count(dep)) return "step '" + id + "' depends on unknown step '" + dep + "'";
                if (dep == id) return "step '" + id + "' depends on itself";
                g[id].push_back(dep);
            }
        std::map<std::string, int> mark;   // 1 visiting, 2 done
        std::string cyc;
        std::function<void(const std::string &)> visit = [&](const std::string & n) {
            if (!cyc.empty()) return;
            mark[n] = 1;
            for (auto & d : g[n]) {
                if (mark[d] == 1) { cyc = n + " and " + d; return; }
                if (mark[d] == 0) visit(d);
            }
            mark[n] = 2;
        };
        for (auto & s : steps) {
            std::string id = s["id"].get<std::string>();
            if (mark[id] == 0) visit(id);
        }
        if (!cyc.empty()) return "the plan has a dependency loop (" + cyc + ")";
    }
    std::vector<std::pair<std::string, std::string>> updates;
    if (has_update) {
        if (!args["update"].is_array()) return "update must be an array of {id, status}";
        for (auto & u : args["update"]) {
            if (!u.is_object() || !u.value("id", "").size()) return "every update needs an id";
            std::string st = u.value("status", "");
            if (!valid_status(st)) return "status must be todo, now, done or repair";
            updates.push_back({u.value("id", ""), st});
        }
    }
    // an update with no plan to apply it to is an error; check against the stored or the new steps
    json trial = doc;
    std::string id;
    json & t = topic_for(trial, name, today, session, &id);
    if (has_steps) t["steps"] = steps;
    if (has_phase) t["phase"] = phase;
    for (auto & [sid, st] : updates) {
        bool found = false;
        for (auto & s : t["steps"])
            if (s.value("id", "") == sid) { s["status"] = st; found = true; }
        if (!found) return "no step '" + sid + "' in this topic's plan";
    }
    doc = trial;
    *ok = true;
    return "saved\n" + render_plan(id, doc["topics"][id]);
}

std::string learn_note_apply(json & doc, const json & args, bool * ok, const std::string & today, const std::string & session) {
    *ok = false;
    if (!args.is_object()) return "arguments must be an object";
    std::string name = trim(args.value("topic", ""));
    if (name.empty() || learn_slug(name).empty()) return "topic is required";
    if (name.size() > 80) return "topic name is too long (80 characters at most)";
    if (!args.contains("notes") || !args["notes"].is_array() || args["notes"].empty())
        return "notes must be an array of 1 to 3 propositions";
    if (args["notes"].size() > 3) return "at most 3 propositions per call; keep them short";
    struct N { std::string text; std::vector<std::string> premises; };
    std::vector<N> in;
    for (auto & n : args["notes"]) {
        N x;
        if (n.is_string()) {
            x.text = trim(n.get<std::string>());
        } else if (n.is_object()) {
            x.text = trim(n.value("text", ""));
            if (n.contains("premises") && !n["premises"].is_null()) {
                if (!n["premises"].is_array()) return "premises must be an array of strings";
                if (n["premises"].size() > 5) return "at most 5 premises per proposition";
                for (auto & p : n["premises"]) {
                    if (!p.is_string()) return "premises must be strings";
                    std::string ps = trim(p.get<std::string>());
                    if (ps.empty()) continue;
                    if (ps.size() > 200) return "a premise is too long (200 characters at most)";
                    x.premises.push_back(ps);
                }
            }
        } else {
            return "each note is {text, premises?} or a plain string";
        }
        if (x.text.empty()) return "a proposition has no text";
        if (x.text.size() > 300) return "a proposition is too long (300 characters at most); say one thing";
        in.push_back(x);
    }
    std::string id;
    json trial = doc;   // nothing is kept when a later premise is refused
    json & t = topic_for(trial, name, today, session, &id);
    json & props = t["props"];
    int next = t.value("next_prop", 1);
    for (auto & x : in) {
        std::string own;   // the proposition this one restates, if any
        for (auto & e : props)
            if (same_claim(e.value("text", ""), x.text)) own = e.value("id", "");
        json prem = json::array();
        for (auto & p : x.premises) {
            std::string ref = learn_premise_ref(props, p);
            std::string pid = lower(trim(p));
            if (!pid.empty() && pid[0] == '#') pid = trim(pid.substr(1));
            if (ref.empty() && is_short_id(pid)) {
                *ok = false;
                return "premise '" + p + "' names no earlier proposition on this topic; use an existing id or state the premise in words";
            }
            if (!ref.empty() && ref == own) continue;   // a claim cannot rest on itself
            if (ref.empty() && same_claim(p, x.text)) continue;   // nor on its own words
            json v = ref.empty() ? p : ref;
            bool dup = false;
            for (auto & q : prem) dup = dup || q == v;
            if (!dup) prem.push_back(v);
        }
        if (!own.empty()) {   // same claim again: refresh its premises
            for (auto & e : props)
                if (e.value("id", "") == own) {
                    e["premises"] = prem;
                    e["date"] = today;
                }
            continue;
        }
        props.push_back({{"id", "p" + std::to_string(next++)}, {"text", x.text}, {"premises", prem}, {"date", today}});
    }
    t["next_prop"] = next;
    while (props.size() > 60) props.erase(props.begin());
    doc = trial;
    *ok = true;
    std::ostringstream o;
    const json & saved = doc["topics"][id];
    o << "saved; " << saved["props"].size() << " propositions on " << saved.value("name", id) << ":\n";
    for (auto & e : saved["props"]) o << "- " << e.value("id", "") << ": " << e.value("text", "") << "\n";
    return o.str();
}

bool learn_progress_migrate(json & doc, const json & progress) {
    if (!doc.is_object()) doc = json::object();
    if (!doc.contains("topics") || !doc["topics"].is_object()) doc["topics"] = json::object();
    if (doc.value("progress_migrated", false)) return false;
    doc["progress_migrated"] = true;
    if (!progress.is_object() || !progress.contains("topics") || !progress["topics"].is_object()) return true;
    for (auto it = progress["topics"].begin(); it != progress["topics"].end(); ++it) {
        const json & o = it.value();
        std::string id = learn_slug(it.key());
        if (id.empty() || !o.is_object() || doc["topics"].contains(id)) continue;
        json steps = json::array();
        bool seen_open = false;
        int n = 0;
        if (o.contains("plan") && o["plan"].is_array())
            for (auto & st : o["plan"]) {
                if (!st.is_object() || n >= 30) continue;
                bool done = st.value("done", false);
                std::string title = trim(st.value("step", ""));
                if (title.empty()) continue;
                n++;
                steps.push_back({{"id", "s" + std::to_string(n)}, {"title", cut(title, 120)},
                                 {"depends_on", n > 1 ? json::array({"s" + std::to_string(n - 1)}) : json::array()},
                                 {"status", done ? "done" : (!seen_open ? "now" : "todo")}});
                if (!done) seen_open = true;
            }
        json props = json::array();
        int next = 1;
        auto add = [&](const std::string & text) {
            std::string t = trim(text);
            if (t.empty() || props.size() >= 60) return;
            props.push_back({{"id", "p" + std::to_string(next++)}, {"text", cut(t, 300)}, {"premises", json::array()},
                             {"date", o.value("updated", "")}});
        };
        if (o.contains("goal") && o["goal"].is_string()) add("Goal: " + o["goal"].get<std::string>());
        if (o.contains("held") && o["held"].is_array())
            for (auto & h : o["held"])
                if (h.is_string()) add("Holds: " + h.get<std::string>());
        if (o.contains("gaps") && o["gaps"].is_array())
            for (auto & g : o["gaps"])
                if (g.is_string()) add("Gap: " + g.get<std::string>());
        if (o.contains("practice") && o["practice"].is_array())
            for (auto & pr : o["practice"])
                if (pr.is_object()) add("Practice " + pr.value("date", "") + ": " + pr.value("problem", "") + " -> " + pr.value("result", ""));
        std::string upd = o.value("updated", "");
        doc["topics"][id] = {{"name", cut(trim(it.key()), 80)}, {"phase", steps.empty() ? "measure" : "teach"}, {"steps", steps},
                             {"props", props}, {"next_prop", next}, {"created", upd}, {"updated", upd}, {"migrated", true}};
    }
    return true;
}

json learn_topics_load_migrated(const Config & cfg) {
    json doc = learn_topics_load(cfg);
    if (doc.value("progress_migrated", false)) return doc;
    json progress = json::object();
    std::ifstream in((fs::path(expand_home(cfg.data_dir)) / "learn" / "progress.json").string());
    bool had_progress = (bool) in;
    if (in) progress = json::parse(in, nullptr, false);
    if (progress.is_discarded()) progress = json::object();
    // Persist only when there was an old record to move; a bare read (or a refused tool call) must not create topics.json.
    if (learn_progress_migrate(doc, progress) && had_progress) learn_topics_save(cfg, doc, nullptr);
    return doc;
}

json learn_topics_list(const json & doc) {
    std::vector<std::pair<std::string, json>> v;
    if (doc.is_object() && doc.contains("topics") && doc["topics"].is_object())
        for (auto it = doc["topics"].begin(); it != doc["topics"].end(); ++it) {
            const json & t = it.value();
            int total = 0, done = 0;
            for (auto & s : t.value("steps", json::array())) {
                total++;
                if (s.value("status", "") == "done") done++;
            }
            v.push_back({t.value("updated", ""), json{{"id", it.key()}, {"name", t.value("name", it.key())},
                                                     {"phase", t.value("phase", "measure")}, {"steps_total", total},
                                                     {"steps_done", done}, {"props", t.value("props", json::array()).size()},
                                                     {"updated", t.value("updated", "")}, {"session", t.value("session", "")}}});
        }
    std::stable_sort(v.begin(), v.end(), [](const auto & a, const auto & b) { return a.first > b.first; });
    json out = json::array();
    for (auto & p : v) out.push_back(p.second);
    return out;
}

json learn_topic_get(const json & doc, const std::string & id) {
    if (!doc.is_object() || !doc.contains("topics") || !doc["topics"].is_object() || !doc["topics"].contains(id)) return nullptr;
    json t = doc["topics"][id];
    t["id"] = id;
    t.erase("next_prop");
    return t;
}

std::string learn_knowledge_digest(const json & doc, size_t max_bytes) {
    json list = learn_topics_list(doc);
    std::string out;
    for (auto & row : list) {
        const json & t = doc["topics"][row["id"].get<std::string>()];
        const json & props = t.value("props", json::array());
        if (props.empty()) continue;
        std::string block = "- " + t.value("name", "") + ":\n";
        size_t from = props.size() > 12 ? props.size() - 12 : 0;
        for (size_t i = from; i < props.size(); i++) {
            block += "  * " + props[i].value("text", "");
            const json & pr = props[i].value("premises", json::array());
            if (!pr.empty()) {
                block += " [because:";
                for (auto & p : pr) {
                    std::string ps = p.get<std::string>(), text = ps;
                    for (auto & q : props)
                        if (q.value("id", "") == ps) text = q.value("text", ps);
                    block += " " + text + ";";
                }
                block.pop_back();
                block += "]";
            }
            block += "\n";
        }
        if (out.size() + block.size() > max_bytes) break;
        out += block;
    }
    return out;
}

}  // namespace judas
