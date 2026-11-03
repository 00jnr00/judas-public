// Swarm teams, the pure parts (see teams.h). Everything here is deterministic and unit tested.

#include "agent/teams.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include <toml++/toml.hpp>

namespace judas {

namespace {

bool valid_name(const std::string & s, size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// The copy suffix ".N": "reader.2" -> base "reader", n 2. No suffix -> base = copy, n 0.
void split_copy(const std::string & copy, std::string & base, int & n) {
    size_t dot = copy.rfind('.');
    n = 0;
    base = copy;
    if (dot == std::string::npos || dot + 1 >= copy.size()) return;
    for (size_t i = dot + 1; i < copy.size(); i++)
        if (copy[i] < '0' || copy[i] > '9') return;
    base = copy.substr(0, dot);
    n = std::atoi(copy.c_str() + dot + 1);
}

std::string str_list_err(const json & v, std::vector<std::string> & out, const std::string & key) {
    if (!v.is_array()) return key + " must be a list of strings";
    out.clear();
    for (auto & x : v) {
        if (!x.is_string()) return key + " must be a list of strings";
        out.push_back(x.get<std::string>());
    }
    return "";
}

// TOML -> JSON for the small subset a team spec uses (strings, integers, bools, arrays, tables).
json toml_to_json(const toml::node & n) {
    if (auto * t = n.as_table()) {
        json o = json::object();
        for (auto && [k, v] : *t) o[std::string(k.str())] = toml_to_json(v);
        return o;
    }
    if (auto * a = n.as_array()) {
        json o = json::array();
        for (auto & v : *a) o.push_back(toml_to_json(v));
        return o;
    }
    if (auto * s = n.as_string()) return s->get();
    if (auto * i = n.as_integer()) return i->get();
    if (auto * b = n.as_boolean()) return b->get();
    if (auto * f = n.as_floating_point()) return f->get();
    return nullptr;
}

std::string member_from_json(const json & j, TeamMemberSpec & m) {
    if (!j.is_object()) return "each member must be an object";
    for (auto & [k, v] : j.items()) {
        if (k == "id") {
            if (!v.is_string()) return "member id must be a string";
            m.id = v.get<std::string>();
        } else if (k == "role") {
            if (!v.is_string()) return "member role must be a string";
            m.role = v.get<std::string>();
        } else if (k == "count") {
            if (!v.is_number_integer()) return "member count must be an integer";
            m.count = v.get<int>();
        } else if (k == "skills") {
            if (auto e = str_list_err(v, m.skills, "member skills"); !e.empty()) return e;
        } else if (k == "model") {
            if (!v.is_string()) return "member model must be a string";
            m.model = v.get<std::string>();
        } else {
            return "unknown member key '" + k + "' (allowed: id, role, count, skills, model)";
        }
    }
    return "";
}

std::string edge_from_json(const json & j, TeamEdge & e) {
    if (!j.is_object()) return "each edge must be an object";
    for (auto & [k, v] : j.items()) {
        if (!v.is_string()) return "edge " + k + " must be a string";
        if (k == "from") e.from = v.get<std::string>();
        else if (k == "to") e.to = v.get<std::string>();
        else if (k == "kind") e.kind = v.get<std::string>();
        else return "unknown edge key '" + k + "' (allowed: from, to, kind)";
    }
    return "";
}

}  // namespace

std::string team_spec_from_json(const json & j, TeamSpec & out) {
    if (!j.is_object()) return "a team spec must be an object";
    TeamSpec s;
    for (auto & [k, v] : j.items()) {
        if (k == "name") {
            if (!v.is_string()) return "name must be a string";
            s.name = v.get<std::string>();
        } else if (k == "brief") {
            if (!v.is_string()) return "brief must be a string";
            s.brief = v.get<std::string>();
        } else if (k == "rules") {
            if (auto e = str_list_err(v, s.rules, "rules"); !e.empty()) return e;
        } else if (k == "rule_files") {
            if (auto e = str_list_err(v, s.rule_files, "rule_files"); !e.empty()) return e;
        } else if (k == "members") {
            if (!v.is_array()) return "members must be a list";
            for (auto & mj : v) {
                TeamMemberSpec m;
                if (auto e = member_from_json(mj, m); !e.empty()) return e;
                s.members.push_back(m);
            }
        } else if (k == "edges") {
            if (!v.is_array()) return "edges must be a list";
            for (auto & ej : v) {
                TeamEdge e;
                if (auto err = edge_from_json(ej, e); !err.empty()) return err;
                s.edges.push_back(e);
            }
        } else if (k == "place") {
            if (!v.is_array()) return "place must be a list of rows (lists of copies)";
            for (auto & row : v) {
                std::vector<std::string> r;
                if (auto e = str_list_err(row, r, "each place row"); !e.empty()) return e;
                s.place.push_back(r);
            }
        } else {
            return "unknown team key '" + k + "' (allowed: name, brief, rules, rule_files, members, edges, place)";
        }
    }
    out = std::move(s);
    return "";
}

std::string team_spec_from_toml(const std::string & text, TeamSpec & out) {
    try {
        toml::table t = toml::parse(text);
        return team_spec_from_json(toml_to_json(t), out);
    } catch (const toml::parse_error & e) {
        std::ostringstream os;
        os << "bad TOML: " << e.description() << " (line " << e.source().begin.line << ")";
        return os.str();
    }
}

json team_spec_to_json(const TeamSpec & s) {
    json members = json::array(), edges = json::array();
    for (auto & m : s.members)
        members.push_back({{"id", m.id}, {"role", m.role}, {"count", m.count}, {"skills", m.skills}, {"model", m.model}});
    for (auto & e : s.edges) edges.push_back({{"from", e.from}, {"to", e.to}, {"kind", e.kind}});
    return {{"name", s.name}, {"brief", s.brief}, {"rules", s.rules}, {"rule_files", s.rule_files},
            {"members", members}, {"edges", edges}, {"place", s.place}};
}

std::string validate_team_spec(const TeamSpec & s, int max_members, int creator_depth, int max_depth) {
    if (!valid_name(s.name, 40)) return "team name must be 1 to 40 characters of a-z, 0-9, _ or -";
    if (s.members.empty()) return "a team needs at least one member";
    if (s.brief.size() > 8000) return "brief is too long (8000 characters at most)";
    if (s.rules.size() > 40) return "too many rules (40 at most)";
    if (s.rule_files.size() > 8) return "too many rule_files (8 at most)";
    std::set<std::string> ids;
    int leads = 0, copies = 0;
    for (auto & m : s.members) {
        if (!valid_name(m.id, 32) || m.id == "lead") return "member id '" + m.id + "' must be 1 to 32 of a-z, 0-9, _ or - (and not 'lead')";
        if (!ids.insert(m.id).second) return "member id '" + m.id + "' is used twice";
        if (m.count < 1 || m.count > 8) return "member '" + m.id + "': count must be 1 to 8";
        if (m.model != "gpu" && m.model != "cpu") return "member '" + m.id + "': model must be gpu or cpu";
        if (m.role.empty() || m.role == "review") return "member '" + m.id + "': role '" + m.role + "' is not allowed";
        if (m.role == "lead") {
            leads++;
            if (m.count != 1) return "the team lead '" + m.id + "' must have count 1";
            if (m.model != "gpu") return "the team lead '" + m.id + "' must run on the gpu model";
        }
        if (m.skills.size() > 6) return "member '" + m.id + "': 6 skills at most";
        copies += m.count;
    }
    if (leads > 1) return "a team has at most one member with role lead";
    if (copies > max_members)
        return "the team has " + std::to_string(copies) + " member copies; the limit left for this session is " +
               std::to_string(max_members);
    int deepest = creator_depth + (leads ? 2 : 1);
    if (deepest > max_depth)
        return "a team " + std::string(leads ? "with a team lead " : "") + "would nest " + std::to_string(deepest) +
               " levels deep; agents.max_depth is " + std::to_string(max_depth) +
               (leads ? " (drop the team lead, or raise max_depth)" : "");
    for (auto & e : s.edges) {
        if (!ids.count(e.from) || !ids.count(e.to)) return "edge " + e.from + " -> " + e.to + " names an unknown member";
        if (e.kind != "message" && e.kind != "observe") return "edge kind must be message or observe";
    }
    std::set<std::string> placed;
    for (auto & row : s.place)
        for (auto & c : row) {
            if (team_member_of(s, c).empty()) return "place names '" + c + "', which is not a copy in the team";
            if (!placed.insert(c).second) return "place names '" + c + "' twice";
        }
    return "";
}

std::string team_lead_of(const TeamSpec & s) {
    for (auto & m : s.members)
        if (m.role == "lead") return m.id;
    return "";
}

std::vector<std::string> team_copies(const TeamMemberSpec & m) {
    if (m.count <= 1) return {m.id};
    std::vector<std::string> out;
    for (int i = 1; i <= m.count; i++) out.push_back(m.id + "." + std::to_string(i));
    return out;
}

std::vector<std::string> team_all_copies(const TeamSpec & s) {
    std::vector<std::string> out;
    for (auto & m : s.members)
        for (auto & c : team_copies(m)) out.push_back(c);
    return out;
}

const TeamMemberSpec * team_member(const TeamSpec & s, const std::string & member_id) {
    for (auto & m : s.members)
        if (m.id == member_id) return &m;
    return nullptr;
}

std::string team_member_of(const TeamSpec & s, const std::string & copy) {
    if (auto * m = team_member(s, copy); m && m->count <= 1) return m->id;
    std::string base;
    int n = 0;
    split_copy(copy, base, n);
    auto * m = team_member(s, base);
    if (!m || m->count <= 1 || n < 1 || n > m->count) return "";
    return m->id;
}

std::string team_parent_of(const TeamSpec & s, const std::string & copy) {
    std::string lead = team_lead_of(s);
    std::string mem = team_member_of(s, copy);
    if (lead.empty() || mem == lead) return "lead";
    return lead;
}

bool edge_allows(const TeamSpec & s, const std::string & from, const std::string & to, const std::string & kind) {
    if (from == to) return false;
    if (kind != "message" && kind != "observe") return false;
    std::string tl = team_lead_of(s);
    std::string fm = from == "lead" ? "lead" : team_member_of(s, from);
    std::string tm = to == "lead" ? "lead" : team_member_of(s, to);
    if (fm.empty() || tm.empty()) return false;
    if (fm == "lead") {
        if (kind == "observe") return true;        // the session lead supervises everyone
        return tl.empty() ? true : tm == tl;       // with a team lead, work goes through it
    }
    if (!tl.empty() && fm == tl) {
        if (tm == "lead") return kind == "message";   // the team lead reports up
        return true;                                  // and directs (and observes) its members
    }
    // a plain member: report to its parent
    if (kind == "message" && to == team_parent_of(s, from)) return true;
    for (auto & e : s.edges)
        if (e.kind == kind && e.from == fm && e.to == tm) return true;
    return false;
}

std::string team_rule_path(const std::string & ref) {
    size_t h = ref.find('#');
    return h == std::string::npos ? ref : ref.substr(0, h);
}

std::string team_rule_text(const std::string & ref, const std::string & file_text, size_t max_chars) {
    std::string out;
    size_t h = ref.find('#');
    if (h == std::string::npos) {
        out = file_text;
    } else {
        std::string want = ref.substr(h + 1);
        auto norm = [](std::string s) {
            size_t a = s.find_first_not_of(" \t#"), b = s.find_last_not_of(" \t\r");
            s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
            return s;
        };
        std::string target = norm(want);
        std::istringstream in(file_text);
        int level = 0;
        bool inside = false;
        for (std::string line; std::getline(in, line);) {
            int lv = 0;
            while (lv < (int) line.size() && line[lv] == '#') lv++;
            bool heading = lv > 0 && lv < (int) line.size() && line[lv] == ' ';
            if (inside && heading && lv <= level) break;
            if (!inside && heading) {
                std::string title = norm(line.substr(lv));
                // match "House rules" against "House rules (apply to EVERY function ...)" too
                if (title == target || title.rfind(target + " ", 0) == 0 || title.rfind(target + "(", 0) == 0 ||
                    title.rfind(target + ":", 0) == 0) {
                    inside = true;
                    level = lv;
                }
            }
            if (inside) out += line + "\n";
        }
        if (!inside) return "";
    }
    if (out.size() > max_chars) {
        size_t cut = max_chars;
        while (cut > 0 && ((unsigned char) out[cut] & 0xC0) == 0x80) cut--;
        out = out.substr(0, cut) + "\n[cut: the rest of " + team_rule_path(ref) + " is in the file]";
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

std::string team_block(const TeamBlockInput & in) {
    const TeamSpec & s = *in.spec;
    std::string mem = team_member_of(s, in.copy);
    const TeamMemberSpec * m = team_member(s, mem);
    std::string role = m ? m->role : "worker";
    std::string parent = team_parent_of(s, in.copy);
    std::string b = std::string(TEAM_BLOCK_PREFIX) + s.name + "] You are " + in.copy + " (" +
                    (role == "lead" ? "team lead" : role) + ") in team " + s.name + ", reporting to " +
                    (parent == "lead" ? "the session lead" : parent) + ".\n";
    b += "This block is kept up to date by the harness and survives context compaction. It is the authority on "
         "the brief and the rules: follow it even if a hand-off message says something different.\n";
    if (!s.brief.empty()) b += "\nBrief:\n" + s.brief + "\n";
    if (!in.rules_text.empty())
        b += "\nHouse rules (verbatim from the source; they apply to every piece of work you do):\n" + in.rules_text + "\n";
    if (!in.request.empty()) {
        std::string r = in.request;
        if (r.size() > in.request_max) {
            size_t cut = in.request_max;
            while (cut > 0 && ((unsigned char) r[cut] & 0xC0) == 0x80) cut--;
            r = r.substr(0, cut) + " [cut]";
        }
        b += "\nThe user's request to the session lead (verbatim, for context; your own task is what you were "
             "assigned):\n" + r + "\n";
    }
    std::vector<std::string> msg, obs;
    std::vector<std::string> everyone = team_all_copies(s);
    everyone.insert(everyone.begin(), "lead");
    for (auto & c : everyone) {
        if (c == in.copy) continue;
        if (edge_allows(s, in.copy, c, "message")) msg.push_back(c == "lead" ? "lead (the session lead)" : c);
        if (edge_allows(s, in.copy, c, "observe")) obs.push_back(c);
    }
    auto join = [](const std::vector<std::string> & v) {
        std::string o;
        for (auto & x : v) o += (o.empty() ? "" : ", ") + x;
        return o;
    };
    b += "\nTeam tools: team_message to " + (msg.empty() ? std::string("nobody") : join(msg));
    if (!obs.empty()) b += "; team_read on " + join(obs);
    b += ". Your final answer (done) is your report to " + (parent == "lead" ? std::string("the session lead") : parent) + ".";
    if (role == "lead")
        b += "\nAs team lead: split your task across your members with team_assign (one complete piece each), "
             "team_wait for their reports, check and merge them, then report up with done. The rules above are "
             "already pinned for every member; do not spend tokens restating them.";
    if (m && m->model == "cpu" && !in.cpu_fallback)
        b += "\nYou run on a small fast model: keep each step short and concrete, read only what your task needs.";
    if (in.cpu_fallback) b += "\nNote: you were meant to run on the CPU swarm model, which is off; you run on the main model.";
    return b;
}

std::string team_status_merge(const std::string & activity, const std::string & engine_phase) {
    if (activity == "queued" || activity == "generating") {
        if (engine_phase == "prefill") return "prefilling";
        if (engine_phase == "generate") return "generating";
        if (engine_phase == "queue") return "queued";
        return activity;
    }
    static const std::set<std::string> known = {"tool", "waiting", "blocked", "done", "failed", "idle"};
    return known.count(activity) ? activity : "idle";
}

}  // namespace judas

// ---------- team map files ----------

namespace judas {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

std::string trim_ws(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "Research Team!" -> "research-team": what a small model meant, in the allowed alphabet.
std::string slug(const std::string & in, size_t max_len) {
    std::string o;
    for (char c : lower(in)) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') o += c;
        else if (!o.empty() && o.back() != '-') o += '-';
    }
    while (!o.empty() && o.back() == '-') o.pop_back();
    while (!o.empty() && o.front() == '-') o.erase(0, 1);
    if (o.size() > max_len) o.resize(max_len);
    return o;
}

std::vector<std::string> words(const std::string & s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

// Text that tries to change what agents may do. Such lines are never taken into a team.
bool touches_permissions(const std::string & text) {
    std::string t = lower(text);
    for (const char * w : {"permission", "sandbox", "sudo", "password", "approval", "full-auto", "allow_paths",
                           "deny_paths", "doas", "pkexec", "faillock"})
        if (t.find(w) != std::string::npos) return true;
    return false;
}

std::vector<std::string> split_csv(const std::string & v) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : v + ",") {
        if (c == ',' || c == ';') {
            if (auto t = trim_ws(cur); !t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

}  // namespace

TeamMap parse_team_map(const std::string & text, const std::string & default_name) {
    TeamMap out;
    struct Pending {
        int line;
        TeamEdge e;
    };
    struct PlaceLine {
        int line;
        size_t row;
        std::vector<std::string> items;
    };
    std::vector<std::vector<Pending>> links;
    std::vector<std::vector<PlaceLine>> places;
    auto warn = [&](int ln, const std::string & m) { out.warnings.push_back("line " + std::to_string(ln) + ": " + m); };
    auto cur = [&]() -> TeamSpec & {
        if (out.teams.empty()) {
            TeamSpec t;
            t.name = slug(default_name, 40);
            if (t.name.empty()) t.name = "team";
            out.teams.push_back(t);
            links.emplace_back();
            places.emplace_back();
        }
        return out.teams.back();
    };
    std::istringstream in(text);
    int ln = 0;
    for (std::string raw; std::getline(in, raw);) {
        ln++;
        std::string line = trim_ws(raw);
        // a comment is a # at the start or after a space ("SPEC.md#House rules" keeps its #)
        for (size_t i = 0; i < line.size(); i++)
            if (line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
                line = trim_ws(line.substr(0, i));
                break;
            }
        while (!line.empty() && (line[0] == '-' || line[0] == '*' || line[0] == '>')) line = trim_ws(line.substr(1));
        if (line.empty() || line.rfind("```", 0) == 0) continue;
        size_t sp = line.find_first_of(" \t:");
        std::string kw = lower(line.substr(0, sp));
        std::string rest = sp == std::string::npos ? "" : trim_ws(line.substr(sp));
        if (!rest.empty() && rest[0] == ':' && kw != "place") rest = trim_ws(rest.substr(1));
        if (kw == "team" || kw == "pod") {
            std::string name = slug(rest, 40);
            if (name.empty()) {
                warn(ln, "team needs a name; skipped");
                continue;
            }
            // the first team line names the implicit team when it has no members yet (a brief or rules written
            // above it are a header for that team)
            if (out.teams.size() == 1 && out.teams[0].members.empty() && links[0].empty() && places[0].empty()) {
                out.teams[0].name = name;
                continue;
            }
            TeamSpec t;
            t.name = name;
            out.teams.push_back(t);
            links.emplace_back();
            places.emplace_back();
            continue;
        }
        if (kw == "brief" || kw == "goal" || kw == "culture") {
            if (rest.empty()) continue;
            if (touches_permissions(rest)) {
                warn(ln, "a team cannot change permissions or approvals; skipped");
                continue;
            }
            TeamSpec & t = cur();
            t.brief += (t.brief.empty() ? "" : "\n") + rest;
            continue;
        }
        if (kw == "rule") {
            if (rest.empty()) continue;
            if (touches_permissions(rest)) {
                warn(ln, "a team cannot change permissions or approvals; skipped");
                continue;
            }
            cur().rules.push_back(rest);
            continue;
        }
        if (kw == "rules" || kw == "rule_file" || kw == "rulefile" || kw == "rule_files" || kw == "file") {
            if (rest.empty()) continue;
            cur().rule_files.push_back(rest);
            continue;
        }
        if (kw == "lead" || kw == "member" || kw == "members" || kw == "worker" || kw == "reviewer") {
            auto ws = words(rest);
            if (ws.empty()) {
                warn(ln, kw + " needs an id; skipped");
                continue;
            }
            TeamMemberSpec m;
            m.id = slug(ws[0], 32);
            if (m.id.empty() || m.id == "lead") {
                warn(ln, "'" + ws[0] + "' is not a usable member id; skipped");
                continue;
            }
            if (m.id != lower(ws[0])) warn(ln, "member id '" + ws[0] + "' written as '" + m.id + "'");
            m.role = kw == "lead" ? "lead" : kw == "reviewer" ? "reviewer" : "worker";
            bool skip = false;
            for (size_t i = 1; i < ws.size(); i++) {
                std::string w = lower(ws[i]);
                size_t eq = w.find('=');
                std::string k = eq == std::string::npos ? w : w.substr(0, eq), v = eq == std::string::npos ? "" : ws[i].substr(eq + 1);
                if (touches_permissions(w) || k == "tools" || k == "tool") {
                    warn(ln, "'" + ws[i] + "' ignored: a team cannot change tools or permissions");
                } else if ((w[0] == 'x' || w[0] == '*') && w.size() > 1 && std::all_of(w.begin() + 1, w.end(), ::isdigit)) {
                    m.count = std::atoi(w.c_str() + 1);
                } else if (std::all_of(w.begin(), w.end(), ::isdigit)) {
                    m.count = std::atoi(w.c_str());
                } else if (k == "count" || k == "copies") {
                    m.count = std::atoi(v.c_str());
                } else if (w == "gpu" || w == "cpu") {
                    m.model = w;
                } else if (k == "model") {
                    m.model = lower(v);
                } else if (k == "role") {
                    m.role = lower(v) == "team-lead" ? "lead" : lower(v);
                } else if (k == "skills" || k == "skill") {
                    m.skills = split_csv(v);
                } else {
                    warn(ln, "'" + ws[i] + "' not understood; ignored");
                }
            }
            if (m.count < 1 || m.count > 8) {
                warn(ln, "count must be 1 to 8; using " + std::string(m.count < 1 ? "1" : "8"));
                m.count = std::clamp(m.count, 1, 8);
            }
            if (m.model != "gpu" && m.model != "cpu") {
                warn(ln, "model must be gpu or cpu; using gpu");
                m.model = "gpu";
            }
            if (m.role == "lead") m.count = 1, m.model = "gpu";
            TeamSpec & t = cur();
            for (auto & x : t.members)
                if (x.id == m.id) skip = true;
            if (skip) {
                warn(ln, "member '" + m.id + "' already exists; skipped");
                continue;
            }
            if (m.role == "lead" && !team_lead_of(t).empty()) {
                warn(ln, "team " + t.name + " already has a lead; '" + m.id + "' becomes a worker");
                m.role = "worker";
            }
            t.members.push_back(m);
            continue;
        }
        if (kw == "link" || kw == "edge") {
            // one pass: every arrow (->, -->, =>) becomes a separate " -> " word
            std::string r;
            for (size_t i = 0; i < rest.size();) {
                if (rest.compare(i, 3, "-->") == 0) { r += " -> "; i += 3; }
                else if (rest.compare(i, 2, "->") == 0 || rest.compare(i, 2, "=>") == 0) { r += " -> "; i += 2; }
                else r += rest[i++];
            }
            auto ws = words(r);
            std::vector<std::string> parts;
            for (auto & w : ws)
                if (w != "->" && lower(w) != "to") parts.push_back(w);
            if (parts.size() < 2) {
                warn(ln, "link needs FROM -> TO; skipped");
                continue;
            }
            TeamEdge e;
            e.from = slug(parts[0], 32);
            e.to = slug(parts[1], 32);
            if (parts.size() > 2) {
                std::string k = lower(parts[2]);
                if (k == "observe" || k == "observes" || k == "watch" || k == "read") e.kind = "observe";
                else if (k == "message" || k == "messages" || k == "talk") e.kind = "message";
                else warn(ln, "link kind '" + parts[2] + "' unknown; using message");
            }
            cur();
            links.back().push_back({ln, e});
            continue;
        }
        if (kw == "place" || kw == "row") {
            std::string r = rest;
            size_t colon = r.find(':');
            std::string head = colon == std::string::npos ? "" : trim_ws(r.substr(0, colon));
            std::string tail = colon == std::string::npos ? r : r.substr(colon + 1);
            auto ws = words(tail);
            if (colon == std::string::npos && !ws.empty() && std::all_of(ws[0].begin(), ws[0].end(), ::isdigit)) {
                head = ws[0];
                ws.erase(ws.begin());
            }
            int row = std::atoi(head.c_str());
            if (row < 1 || row > 12) {
                warn(ln, "place needs a row number 1 to 12 (place 2: a b c); skipped");
                continue;
            }
            cur();
            places.back().push_back({ln, (size_t) row, ws});
            continue;
        }
        warn(ln, "unknown keyword '" + kw + "' (use team, brief, rule, rules, lead, member, link, place); skipped");
    }
    // links and places need the full member list of their team
    for (size_t ti = 0; ti < out.teams.size(); ti++) {
        TeamSpec & t = out.teams[ti];
        for (auto & pl : links[ti]) {
            if (!team_member(t, pl.e.from) || !team_member(t, pl.e.to)) {
                warn(pl.line, "link " + pl.e.from + " -> " + pl.e.to + " names a member that is not in team " + t.name + "; skipped");
                continue;
            }
            bool dup = false;
            for (auto & e : t.edges) dup |= e.from == pl.e.from && e.to == pl.e.to && e.kind == pl.e.kind;
            if (!dup) t.edges.push_back(pl.e);
        }
        std::set<std::string> placed;
        std::map<size_t, std::vector<std::string>> rows;
        for (auto & pl : places[ti]) {
            for (auto & item : pl.items) {
                std::string c = lower(item);
                while (!c.empty() && (c.back() == ',' || c.back() == ';')) c.pop_back();
                std::vector<std::string> expand;
                if (const TeamMemberSpec * m = team_member(t, c); m && m->count > 1) expand = team_copies(*m);
                else if (!team_member_of(t, c).empty()) expand = {c};
                if (expand.empty()) {
                    warn(pl.line, "'" + item + "' is not a copy in team " + t.name + "; skipped");
                    continue;
                }
                for (auto & x : expand)
                    if (placed.insert(x).second) rows[pl.row].push_back(x);
            }
        }
        for (auto & [r, v] : rows)
            if (!v.empty()) t.place.push_back(v);   // row numbers only order the rows; gaps close up
    }
    return out;
}

std::string team_map_text(const TeamSpec & s) {
    std::string o = "team " + s.name + "\n";
    if (!s.brief.empty()) {
        std::istringstream in(s.brief);
        for (std::string l; std::getline(in, l);)
            if (auto t = trim_ws(l); !t.empty()) o += "brief " + t + "\n";
    }
    for (auto & r : s.rule_files) o += "rules " + r + "\n";
    for (auto & r : s.rules) o += "rule " + r + "\n";
    for (auto & m : s.members) {
        std::string skills;
        for (auto & k : m.skills) skills += (skills.empty() ? "" : ",") + k;
        if (m.role == "lead") {
            o += "lead " + m.id + (skills.empty() ? "" : " skills=" + skills) + "\n";
            continue;
        }
        o += "member " + m.id;
        if (m.count > 1) o += " x" + std::to_string(m.count);
        if (m.model != "gpu") o += " " + m.model;
        if (m.role != "worker") o += " role=" + m.role;
        if (!skills.empty()) o += " skills=" + skills;
        o += "\n";
    }
    for (auto & e : s.edges) o += "link " + e.from + " -> " + e.to + (e.kind == "message" ? "" : " " + e.kind) + "\n";
    for (size_t i = 0; i < s.place.size(); i++) {
        o += "place " + std::to_string(i + 1) + ":";
        for (auto & c : s.place[i]) o += " " + c;
        o += "\n";
    }
    return o;
}

}  // namespace judas
