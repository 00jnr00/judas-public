// Swarm team view: parsing the /team body, the text map, hand-off notes, row lookup, /read bounds.
#include <cstdlib>
#include <fstream>

#include "core/json.h"
#include "test.h"
#include "tui/team.h"

using namespace judas::tui;
using judas::json;

static json agent(const char * id, const char * team, const char * member, const char * act, double ctx, const char * last,
                  const char * role = "worker", const char * tool = "", double tps = 0, const char * model = "gpu") {
    return {{"id", id}, {"parent", "ag-lead"}, {"role", role}, {"team", team}, {"member", member}, {"activity", act}, {"tool", tool},
            {"model", model}, {"tok_s", tps}, {"ctx_pct", ctx}, {"last_output", last}, {"result", ""},
            {"prompt_tokens", 4000}, {"completion_tokens", 200}};
}

static json fixture() {
    json b;
    b["session"] = "S";
    b["lead"] = agent("ag-lead", "", "", "waiting", 31, "", "lead");
    b["agents"] = json::array({
        agent("ag-ed", "research", "editor", "prefilling", 48, "Merging the first two reports into REPORT.md", "lead"),
        agent("ag-r1", "research", "reader", "generating", 72, "Prefix sharing is not used; every request prefills its own prompt.", "worker", "", 96.4),
        agent("ag-r2", "research", "reader", "tool", 24, "fetching", "worker", "web_fetch"),
        agent("ag-r3", "research", "reader", "failed", 93, "I cannot read this file."),
        agent("ag-ru1", "ops", "runner", "done", 85, "Throughput script finished.")});
    b["teams"] = json::array({
        {{"name", "research"}, {"brief", "Answer from primary sources.\nSecond line."}, {"lead_member", "editor"}, {"disbanded", false},
         {"members", json::array({{{"id", "editor"}, {"role", "lead"}, {"count", 1}, {"model", "gpu"}},
                                  {{"id", "reader"}, {"role", "worker"}, {"count", 3}, {"model", "gpu"}}})},
         {"copies", json::array({{{"copy", "editor"}, {"member", "editor"}, {"agent", "ag-ed"}},
                                 {{"copy", "reader.1"}, {"member", "reader"}, {"agent", "ag-r1"}},
                                 {{"copy", "reader.2"}, {"member", "reader"}, {"agent", "ag-r2"}},
                                 {{"copy", "reader.3"}, {"member", "reader"}, {"agent", "ag-r3"}}})}},
        {{"name", "ops"}, {"brief", ""}, {"lead_member", ""}, {"disbanded", false},
         {"members", json::array({{{"id", "runner"}, {"role", "worker"}, {"count", 2}, {"model", "gpu"}}})},
         {"copies", json::array({{{"copy", "runner.1"}, {"member", "runner"}, {"agent", "ag-ru1"}},
                                 {{"copy", "runner.2"}, {"member", "runner"}, {"agent", ""}}})}}});
    return b;
}

static std::string joined(const std::vector<std::string> & rows) {
    std::string o;
    for (auto & r : rows) o += strip_ansi(r) + "\n";
    return o;
}

int main() {
    Palette plain = Palette::plain();
    TeamView v = parse_team(fixture());
    REQUIRE(v.lead.has_value());
    CHECK(v.teams.size() == 2);
    CHECK(v.count == 6);
    // team lead first, then failed before working
    auto & r = v.teams[0].rows;
    CHECK(r[0].copy == "editor" && r[0].n == 1);
    CHECK(r[1].copy == "reader.3" && r[1].activity == "failed");
    CHECK(r[2].copy == "reader.1" || r[2].copy == "reader.2");
    CHECK(v.teams[0].brief == "Answer from primary sources.");
    // done sorts after the unspawned (queued) copy; unspawned copies have no agent
    CHECK(v.teams[1].rows[0].copy == "runner.2" && !v.teams[1].rows[0].spawned);
    CHECK(v.teams[1].rows[1].activity == "done");

    CHECK(team_glyph("generating") == "\xE2\x97\x8F");
    CHECK(team_glyph("blocked") == "\xE2\x97\x90");
    CHECK(team_glyph("failed") == "\xE2\x9C\x95");
    CHECK(team_glyph("queued") == "\xE2\x97\x8B");
    CHECK(team_word("tool") == "working");

    Palette col = plain;
    col.ok = "<ok>"; col.warn = "<warn>"; col.err = "<err>"; col.dim = "<dim>";
    CHECK(ctx_color(col, 59) == "<ok>");
    CHECK(ctx_color(col, 60) == "<warn>");
    CHECK(ctx_color(col, 80) == "<warn>");
    CHECK(ctx_color(col, 81) == "<err>");
    CHECK(state_color(col, "failed") == "<err>");
    CHECK(state_color(col, "blocked") == "<warn>");

    for (int w : {80, 100, 140}) {
        auto rows = render_team(v, plain, w);
        for (auto & line : rows) CHECK(display_width(line) <= w - 1);
    }
    std::string t80 = joined(render_team(v, plain, 80));
    CHECK(t80.find("lead") != std::string::npos);
    CHECK(t80.find("research  Answer from primary sources.") != std::string::npos);
    CHECK(t80.find("reader.2") != std::string::npos && t80.find("tool: web_fetch") != std::string::npos);
    CHECK(t80.find("96 t/s") != std::string::npos);
    std::string t140 = joined(render_team(v, plain, 140));
    CHECK(t140.find("every request prefills its own prompt.") != std::string::npos);
    CHECK(t80.find("every request prefills its own prompt.") == std::string::npos);  // cut at 80

    CHECK(parse_team(json::object()).empty());
    CHECK(joined(render_team(parse_team(json::object()), plain, 80)).find("no teams yet") != std::string::npos);

    // hand-off notes
    auto ev = [](const char * a, json extra) { json d = extra; d["action"] = a; return json{{"type", "team"}, {"data", d}}; };
    CHECK(*handoff_note(ev("assigned", {{"from", "editor"}, {"to", "reader.2"}, {"copy", "reader.2"}})) == "editor -> reader.2 assigned");
    CHECK(*handoff_note(ev("report", {{"from", "reader.1"}, {"to", "editor"}})) == "reader.1 -> editor report");
    CHECK(*handoff_note(ev("message", {{"from", ""}, {"to", "editor"}})) == "lead -> editor message");
    CHECK(*handoff_note(ev("created", {{"team", "ops"}})) == "team ops created");
    CHECK(!handoff_note(json{{"type", "activity"}, {"data", {{"activity", "tool"}}}}).has_value());
    CHECK(!handoff_note(ev("bogus", {})).has_value());

    // lookup
    CHECK(find_row(v, "2") && find_row(v, "2")->copy == "reader.3");
    CHECK(find_row(v, "reader.2") && find_row(v, "reader.2")->agent == "ag-r2");
    CHECK(find_row(v, "ag-r1") && find_row(v, "ag-r1")->copy == "reader.1");
    CHECK(find_row(v, "lead") && find_row(v, "lead")->agent == "ag-lead");
    CHECK(find_row(v, "editor") && find_row(v, "editor")->n == 1);   // unique member
    CHECK(find_row(v, "reader") == nullptr);                          // ambiguous
    CHECK(find_row(v, "99") == nullptr);
    CHECK(find_row(v, "") == nullptr);

    // /read bounds
    TeamRow big;
    big.last = std::string(100, 'a');
    big.result = std::string(5000, 'b');
    big.n = 3;
    std::string out = agent_output_text(big, 4000);
    CHECK(out.rfind(std::string(4000, 'b'), 0) == 0);
    CHECK(out.find("more chars") != std::string::npos);
    CHECK(agent_output_text(big, 6000).size() == 5000);
    TeamRow u;
    u.last = "h\xC3\xA9llo";   // cut inside the 2-byte e-acute must back up
    CHECK(agent_output_text(u, 2).rfind("h", 0) == 0 && agent_output_text(u, 2).find("\xC3\n") == std::string::npos);
    std::string key; size_t num;
    split_key_arg("reader.2 500", key, num);
    CHECK(key == "reader.2" && num == 500);
    split_key_arg("  3 ", key, num);
    CHECK(key == "3" && num == 0);
    split_key_arg("", key, num);
    CHECK(key.empty());

    if (const char * d = std::getenv("JUDAS_TEAM_RENDER")) {   // visual check helper
        for (int w : {80, 120}) {
            std::ofstream f(std::string(d) + "/team-" + std::to_string(w) + ".txt");
            for (auto & l : joined(render_team(v, plain, w)).empty() ? std::vector<std::string>{} : render_team(v, Palette::from_skin("judas"), w)) f << l << Palette::from_skin("judas").reset << "\n";
        }
    }
    TEST_MAIN_END();
}
