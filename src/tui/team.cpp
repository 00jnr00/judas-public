#include "tui/team.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace judas::tui {

using judas::json;

static std::string js(const json & j, const char * k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

static std::string first_line(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find('\n', a);
    std::string l = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
    return l;
}

static bool working(const std::string & a) { return a == "generating" || a == "prefilling" || a == "tool"; }

static int rank(const std::string & a) {
    if (a == "failed" || a == "blocked") return 0;
    if (working(a)) return 1;
    if (a == "waiting" || a == "paused") return 2;
    if (a == "done") return 4;
    return 3;
}

std::string team_glyph(const std::string & a) {
    if (working(a) || a == "done") return "\xE2\x97\x8F";                          // ●
    if (a == "waiting" || a == "blocked" || a == "paused") return "\xE2\x97\x90";    // ◐
    if (a == "failed") return "\xE2\x9C\x95";                                       // ✕
    return "\xE2\x97\x8B";                                                          // ○
}

std::string team_word(const std::string & a) {
    if (working(a)) return "working";
    if (a == "cancelled") return "stopped";
    return a.empty() ? "idle" : a;
}

std::string state_color(const Palette & p, const std::string & a) {
    if (working(a) || a == "done") return p.ok;
    if (a == "waiting" || a == "blocked" || a == "paused") return p.warn;
    if (a == "failed") return p.err;
    return p.dim;
}

std::string ctx_color(const Palette & p, int pct) {
    if (pct < 0) return p.dim;
    return pct < 60 ? p.ok : pct <= 80 ? p.warn : p.err;
}

static TeamRow row_of(const json & a) {
    TeamRow r;
    r.agent = js(a, "id");
    r.team = js(a, "team");
    r.member = js(a, "member");
    r.role = js(a, "role");
    r.model = js(a, "model");
    r.activity = js(a, "activity");
    if (r.activity.empty()) r.activity = js(a, "status");
    r.tool = js(a, "tool");
    r.last = js(a, "last_output");
    r.result = js(a, "result");
    r.tok_s = a.contains("tok_s") && a["tok_s"].is_number() ? a["tok_s"].get<double>() : 0;
    r.ctx_pct = a.contains("ctx_pct") && a["ctx_pct"].is_number() ? (int) (a["ctx_pct"].get<double>() + 0.5) : -1;
    r.tokens = (a.contains("prompt_tokens") && a["prompt_tokens"].is_number() ? a["prompt_tokens"].get<long long>() : 0) +
               (a.contains("completion_tokens") && a["completion_tokens"].is_number() ? a["completion_tokens"].get<long long>() : 0);
    return r;
}

TeamView parse_team(const json & body) {
    TeamView v;
    if (body.contains("lead") && body["lead"].is_object()) {
        v.lead = row_of(body["lead"]);
        v.lead->copy = "lead";
    }
    std::map<std::string, json> by_id;
    std::vector<json> loose;
    if (body.contains("agents") && body["agents"].is_array())
        for (auto & a : body["agents"]) {
            by_id[js(a, "id")] = a;
            if (js(a, "team").empty()) loose.push_back(a);
        }
    std::vector<TeamBlock> blocks;
    if (body.contains("teams") && body["teams"].is_array()) {
        for (auto & t : body["teams"]) {
            TeamBlock b;
            b.name = js(t, "name");
            b.brief = first_line(js(t, "brief"));
            b.disbanded = t.value("disbanded", false);
            std::string lm = js(t, "lead_member");
            std::map<std::string, json> members;
            if (t.contains("members") && t["members"].is_array())
                for (auto & m : t["members"]) members[js(m, "id")] = m;
            auto add = [&](const std::string & copy, const std::string & member, const std::string & agent) {
                TeamRow r;
                auto it = by_id.find(agent);
                if (!agent.empty() && it != by_id.end()) r = row_of(it->second);
                else {
                    r.spawned = false;
                    r.activity = "queued";
                    auto m = members.find(member);
                    if (m != members.end()) { r.role = js(m->second, "role"); r.model = js(m->second, "model"); }
                }
                r.team = b.name;
                r.copy = copy;
                r.member = member;
                r.team_lead = !lm.empty() && member == lm;
                b.rows.push_back(r);
            };
            if (t.contains("copies") && t["copies"].is_array() && !t["copies"].empty()) {
                for (auto & c : t["copies"]) add(js(c, "copy"), js(c, "member"), js(c, "agent"));
            } else {
                for (auto & m : members) {
                    int n = m.second.value("count", 1);
                    for (int i = 1; i <= n; i++) add(n > 1 ? m.first + "." + std::to_string(i) : m.first, m.first, "");
                }
            }
            blocks.push_back(std::move(b));
        }
    }
    if (!loose.empty()) {
        TeamBlock b;
        b.name = "(no team)";
        for (auto & a : loose) {
            if (v.lead && js(a, "id") == v.lead->agent) continue;
            TeamRow r = row_of(a);
            r.copy = r.agent.substr(0, 8);
            b.rows.push_back(r);
        }
        if (!b.rows.empty()) blocks.push_back(std::move(b));
    }
    int n = 0;
    for (auto & b : blocks) {
        std::stable_sort(b.rows.begin(), b.rows.end(), [](const TeamRow & x, const TeamRow & y) {
            if (x.team_lead != y.team_lead) return x.team_lead;
            return rank(x.activity) < rank(y.activity);
        });
        for (auto & r : b.rows) r.n = ++n;
        v.teams.push_back(std::move(b));
    }
    v.count = n;
    return v;
}

static std::string pad(const std::string & s, int w) {
    int d = display_width(s);
    return d >= w ? s : s + std::string((size_t) (w - d), ' ');
}
static std::string rpad(const std::string & s, int w) {
    int d = display_width(s);
    return d >= w ? s : std::string((size_t) (w - d), ' ') + s;
}

static std::string row_line(const TeamRow & r, const Palette & pal, int width, int copy_w, bool indent) {
    std::string num = r.n > 0 ? rpad(std::to_string(r.n), 2) : "  ";
    std::string role = r.role == "worker" ? "worker" : r.role;
    std::string model = r.model.empty() ? "   " : r.model;
    std::string ctx = r.ctx_pct >= 0 ? std::to_string(r.ctx_pct) + "%" : "-";
    std::string toks = r.spawned ? human_tokens(r.tokens) : "";
    char tps[16] = "";
    if (r.tok_s >= 0.5) std::snprintf(tps, sizeof tps, "%.0f t/s", r.tok_s);
    std::string tail = !r.tool.empty() && r.activity == "tool" ? "tool: " + r.tool : one_line(r.last);
    std::string head = pal.dim + num + pal.reset + " " + (indent ? "  " : "") + state_color(pal, r.activity) + team_glyph(r.activity) + pal.reset +
                       " " + pal.bold + pad(r.copy, copy_w) + pal.reset + " " + pal.dim + pad(role, 6) + " " + pad(model, 3) + pal.reset + " " +
                       state_color(pal, r.activity) + pad(team_word(r.activity), 7) + pal.reset + " " +
                       ctx_color(pal, r.ctx_pct) + rpad(ctx, 4) + pal.reset + " " + rpad(toks, 5) + " " + rpad(tps, 7);
    int used = 2 + 1 + (indent ? 2 : 0) + 1 + 1 + copy_w + 1 + 6 + 1 + 3 + 1 + 7 + 1 + 4 + 1 + 5 + 1 + 7;
    int room = width - used - 2;
    if (!tail.empty() && room >= 6) head += " " + pal.dim + truncate_width(tail, std::min(room, 200)) + pal.reset;
    return head;
}

std::vector<std::string> render_team(const TeamView & v, const Palette & pal, int width) {
    std::vector<std::string> o;
    if (v.lead) {
        const TeamRow & l = *v.lead;
        char tps[16] = "";
        if (l.tok_s >= 0.5) std::snprintf(tps, sizeof tps, "  %.0f t/s", l.tok_s);
        o.push_back(pal.dim + std::string("   ") + pal.reset + state_color(pal, l.activity) + team_glyph(l.activity) + pal.reset + " " + pal.bold + "lead" + pal.reset +
                    "  " + state_color(pal, l.activity) + team_word(l.activity) + pal.reset + "  " + ctx_color(pal, l.ctx_pct) +
                    (l.ctx_pct >= 0 ? "ctx " + std::to_string(l.ctx_pct) + "%" : std::string("ctx -")) + pal.reset + pal.dim + "  " +
                    human_tokens(l.tokens) + " tok" + tps + pal.reset);
    }
    if (v.teams.empty()) {
        o.push_back(pal.dim + std::string("   no teams yet (the lead makes them with team_create in Swarm mode)") + pal.reset);
        return o;
    }
    int cw = 6;
    for (auto & b : v.teams) for (auto & r : b.rows) cw = std::max(cw, display_width(r.copy));
    cw = std::min(cw, 14);
    for (auto & b : v.teams) {
        std::string h = pal.bold + pal.accent + b.name + pal.reset;
        if (b.disbanded) h += pal.dim + " (disbanded)" + pal.reset;
        int room = width - display_width(b.name) - (b.disbanded ? 12 : 0) - 5;
        if (!b.brief.empty() && room > 6) h += pal.dim + std::string("  ") + truncate_width(b.brief, room) + pal.reset;
        o.push_back("   " + h);
        for (auto & r : b.rows) o.push_back(row_line(r, pal, width, cw, !r.team_lead));
    }
    return o;
}

std::optional<std::string> handoff_note(const json & ev) {
    if (js(ev, "type") != "team" || !ev.contains("data") || !ev["data"].is_object()) return std::nullopt;
    const json & d = ev["data"];
    std::string act = js(d, "action"), team = js(d, "team");
    auto name = [](const std::string & s) { return s.empty() ? std::string("lead") : s; };
    if (act == "assigned") return name(js(d, "from")) + " -> " + (js(d, "to").empty() ? js(d, "copy") : js(d, "to")) + " assigned";
    if (act == "message" || act == "report") return name(js(d, "from")) + " -> " + name(js(d, "to")) + " " + act;
    if (act == "created") return "team " + team + " created";
    if (act == "disbanded") return "team " + team + " disbanded";
    return std::nullopt;
}

const TeamRow * find_row(const TeamView & v, const std::string & key) {
    if (key.empty()) return nullptr;
    if (key == "lead") return v.lead ? &*v.lead : nullptr;
    bool digits = std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (digits) {
        int n = std::atoi(key.c_str());
        for (auto & b : v.teams) for (auto & r : b.rows) if (r.n == n) return &r;
        return nullptr;
    }
    for (auto & b : v.teams) for (auto & r : b.rows) if (r.copy == key || (!r.agent.empty() && r.agent == key)) return &r;
    const TeamRow * hit = nullptr;
    int hits = 0;
    for (auto & b : v.teams)
        for (auto & r : b.rows)
            if (r.member == key || (!r.agent.empty() && r.agent.rfind(key, 0) == 0)) { hit = &r; hits++; }
    return hits == 1 ? hit : nullptr;
}

std::string agent_output_text(const TeamRow & r, size_t max_chars) {
    std::string t = !r.result.empty() && r.result.size() >= r.last.size() ? r.result : r.last;
    if (t.empty()) return "";
    if (t.size() <= max_chars) return t;
    size_t cut = max_chars;
    while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) cut--;
    return t.substr(0, cut) + "\n... (" + std::to_string(t.size() - cut) + " more chars; /read " + (r.n > 0 ? std::to_string(r.n) : r.copy) +
           " " + std::to_string(t.size()) + " prints all)";
}

void split_key_arg(const std::string & arg, std::string & key, size_t & num) {
    num = 0;
    size_t a = arg.find_first_not_of(" \t");
    if (a == std::string::npos) { key.clear(); return; }
    size_t sp = arg.find_first_of(" \t", a);
    key = arg.substr(a, sp == std::string::npos ? std::string::npos : sp - a);
    if (sp != std::string::npos) {
        size_t b = arg.find_first_not_of(" \t", sp);
        if (b != std::string::npos) num = (size_t) std::strtoull(arg.c_str() + b, nullptr, 10);
    }
}

}  // namespace judas::tui
