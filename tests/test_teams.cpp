// Swarm teams, pure parts: spec parsing (JSON and TOML), validation, copies, edges, rule sections, the pinned
// block and the status merge.
#include "agent/teams.h"
#include <unistd.h>

#include <filesystem>

#include "test.h"

using namespace judas;

static TeamSpec research() {
    TeamSpec s;
    std::string e = team_spec_from_json(json::parse(R"({
        "name": "research", "brief": "Answer from sources.", "rules": ["Cite files."],
        "members": [{"id": "editor", "role": "lead"},
                    {"id": "reader", "count": 3, "skills": ["web"]},
                    {"id": "triage", "model": "cpu"}],
        "edges": [{"from": "reader", "to": "reader", "kind": "observe"}]})"), s);
    REQUIRE(e.empty());
    return s;
}

static void test_parse() {
    TeamSpec s = research();
    CHECK(s.members.size() == 3);
    CHECK(s.members[1].count == 3);
    CHECK(s.members[2].model == "cpu");
    CHECK(s.edges.size() == 1 && s.edges[0].kind == "observe");
    TeamSpec bad;
    CHECK(!team_spec_from_json(json::parse(R"({"name":"x","permissions":{"mode":"full-auto"}})"), bad).empty());
    CHECK(!team_spec_from_json(json::parse(R"({"name":"x","members":[{"id":"a","sandbox":"none"}]})"), bad).empty());
    CHECK(!team_spec_from_json(json::parse(R"({"name":"x","members":[{"id":"a","tools":["shell"]}]})"), bad).empty());
    CHECK(!team_spec_from_json(json::parse(R"({"name":"x","rules":"one"})"), bad).empty());
    TeamSpec t;
    std::string e = team_spec_from_toml(R"(
name = "docs"
brief = "Split the book."
rule_files = ["SPEC.md#House rules"]
[[members]]
id = "reader"
count = 2
[[edges]]
from = "reader"
to = "reader"
kind = "message"
)", t);
    CHECK(e.empty());
    CHECK(t.name == "docs" && t.members.size() == 1 && t.members[0].count == 2 && t.rule_files.size() == 1);
    CHECK(!team_spec_from_toml("name = [", t).empty());
    CHECK(!team_spec_from_toml("name = \"a\"\n[permissions]\nmode = \"full-auto\"\n", t).empty());
    json back = team_spec_to_json(research());
    TeamSpec rt;
    CHECK(team_spec_from_json(back, rt).empty() && rt.members.size() == 3 && rt.edges.size() == 1);
}

static void test_validate() {
    TeamSpec s = research();
    CHECK(validate_team_spec(s, 16, 0, 2).empty());
    CHECK(!validate_team_spec(s, 4, 0, 2).empty());        // 5 copies > 4
    CHECK(!validate_team_spec(s, 16, 0, 1).empty());       // team lead needs depth 2
    CHECK(!validate_team_spec(s, 16, 1, 2).empty());       // a team lead creating a team with a lead: too deep
    TeamSpec flat = s;
    flat.members.erase(flat.members.begin());
    CHECK(validate_team_spec(flat, 16, 0, 1).empty());
    TeamSpec dup = flat;
    dup.members.push_back(dup.members[0]);
    CHECK(!validate_team_spec(dup, 16, 0, 2).empty());
    TeamSpec two = s;
    two.members.push_back({"chief", "lead", 1, {}, "gpu"});
    CHECK(!validate_team_spec(two, 16, 0, 2).empty());
    TeamSpec cpulead = s;
    cpulead.members[0].model = "cpu";
    CHECK(!validate_team_spec(cpulead, 16, 0, 2).empty());
    TeamSpec badname = s;
    badname.name = "Bad Name";
    CHECK(!validate_team_spec(badname, 16, 0, 2).empty());
    TeamSpec leadid = flat;
    leadid.members[0].id = "lead";
    CHECK(!validate_team_spec(leadid, 16, 0, 2).empty());
    TeamSpec badedge = s;
    badedge.edges.push_back({"reader", "ghost", "message"});
    CHECK(!validate_team_spec(badedge, 16, 0, 2).empty());
    TeamSpec review = flat;
    review.members[0].role = "review";
    CHECK(!validate_team_spec(review, 16, 0, 2).empty());
}

static void test_copies_and_edges() {
    TeamSpec s = research();
    auto all = team_all_copies(s);
    CHECK(all.size() == 5);
    CHECK(all[0] == "editor" && all[1] == "reader.1" && all[3] == "reader.3" && all[4] == "triage");
    CHECK(team_member_of(s, "reader.2") == "reader");
    CHECK(team_member_of(s, "reader.4").empty());
    CHECK(team_member_of(s, "reader").empty());          // count 3: the bare id is not a copy
    CHECK(team_member_of(s, "editor") == "editor");
    CHECK(team_lead_of(s) == "editor");
    CHECK(team_parent_of(s, "reader.1") == "editor");
    CHECK(team_parent_of(s, "editor") == "lead");
    // session lead -> team lead only (work goes through the team lead), but observes everyone
    CHECK(edge_allows(s, "lead", "editor", "message"));
    CHECK(!edge_allows(s, "lead", "reader.1", "message"));
    CHECK(edge_allows(s, "lead", "reader.1", "observe"));
    // team lead -> members, and reports up
    CHECK(edge_allows(s, "editor", "reader.2", "message"));
    CHECK(edge_allows(s, "editor", "lead", "message"));
    CHECK(!edge_allows(s, "editor", "lead", "observe"));
    // members report to their parent only, plus spec edges
    CHECK(edge_allows(s, "reader.1", "editor", "message"));
    CHECK(!edge_allows(s, "reader.1", "lead", "message"));
    CHECK(edge_allows(s, "reader.1", "reader.3", "observe"));
    CHECK(!edge_allows(s, "reader.1", "reader.3", "message"));
    CHECK(!edge_allows(s, "reader.1", "reader.1", "observe"));
    CHECK(!edge_allows(s, "triage", "reader.1", "observe"));
    CHECK(!edge_allows(s, "reader.1", "ghost", "message"));
    CHECK(!edge_allows(s, "reader.1", "editor", "shout"));
    // no team lead: the session lead directs members, they report to it
    TeamSpec flat = s;
    flat.members.erase(flat.members.begin());
    CHECK(edge_allows(flat, "lead", "reader.2", "message"));
    CHECK(edge_allows(flat, "reader.2", "lead", "message"));
    CHECK(team_parent_of(flat, "reader.2") == "lead");
}

static void test_rule_text() {
    std::string spec = "# Spec\nintro\n\n## House rules (apply to EVERY function in EVERY module)\n- A: strip\n- B: prefix\n"
                       "### detail\nsub\n## Module 1\nbody\n";
    std::string r = team_rule_text("SPEC.md#House rules", spec);
    CHECK(r.find("- A: strip") != std::string::npos);
    CHECK(r.find("sub") != std::string::npos);            // a deeper heading stays inside the section
    CHECK(r.find("Module 1") == std::string::npos);
    CHECK(r.rfind("## House rules", 0) == 0);
    CHECK(team_rule_text("SPEC.md#house RULES", spec) == r);
    CHECK(team_rule_text("SPEC.md#Nope", spec).empty());
    CHECK(team_rule_text("SPEC.md", spec).find("Module 1") != std::string::npos);
    std::string cut = team_rule_text("SPEC.md", std::string(10000, 'x'), 100);
    CHECK(cut.size() < 200 && cut.find("[cut:") != std::string::npos);
    CHECK(team_rule_path("SPEC.md#House rules") == "SPEC.md");
    CHECK(team_rule_path("a/b.md") == "a/b.md");
}

static void test_block() {
    TeamSpec s = research();
    TeamBlockInput in;
    in.spec = &s;
    in.copy = "reader.2";
    in.rules_text = "- Cite files.\n- A: strip";
    in.request = std::string(5000, 'q');
    in.request_max = 100;
    std::string b = team_block(in);
    CHECK(b.rfind(TEAM_BLOCK_PREFIX, 0) == 0);
    CHECK(b.find("reader.2") != std::string::npos);
    CHECK(b.find("reporting to editor") != std::string::npos);
    CHECK(b.find("Answer from sources.") != std::string::npos);
    CHECK(b.find("- A: strip") != std::string::npos);
    CHECK(b.find("[cut]") != std::string::npos);
    CHECK(b.find(std::string(101, 'q')) == std::string::npos);
    CHECK(b.find("team_read on reader.1, reader.3") != std::string::npos);
    CHECK(b.find("\xE2\x80\x94") == std::string::npos);   // no em dash
    in.copy = "editor";
    std::string lb = team_block(in);
    CHECK(lb.find("team lead") != std::string::npos && lb.find("team_assign") != std::string::npos);
    CHECK(lb.find("the session lead") != std::string::npos);
    in.copy = "triage";
    CHECK(team_block(in).find("small fast model") != std::string::npos);
    in.cpu_fallback = true;
    CHECK(team_block(in).find("swarm model, which is off") != std::string::npos);
}

static void test_status() {
    CHECK(team_status_merge("queued", "") == "queued");
    CHECK(team_status_merge("queued", "queue") == "queued");
    CHECK(team_status_merge("queued", "prefill") == "prefilling");
    CHECK(team_status_merge("generating", "prefill") == "prefilling");
    CHECK(team_status_merge("queued", "generate") == "generating");
    CHECK(team_status_merge("tool", "generate") == "tool");
    CHECK(team_status_merge("waiting", "") == "waiting");
    CHECK(team_status_merge("blocked", "") == "blocked");
    CHECK(team_status_merge("done", "") == "done");
    CHECK(team_status_merge("failed", "") == "failed");
    CHECK(team_status_merge("bogus", "") == "idle");
}

static void test_map() {
    TeamMap m = parse_team_map(R"(# research.map
brief Shared goal before any team line.
team Research Team
Brief: Answer from the sources.
brief Write REPORT.md.
rules SPEC.md#House rules   # pinned verbatim
rule Quote numbers exactly.
- lead Editor skills=web
member reader x3
member triage cpu skills=web-research,tags tools=shell
member check role=reviewer
member reader x2
link reader -> reader observe
link reader=>ghost
link editor to triage
place 1: editor
place 3 reader triage
place 2: nobody
frobnicate everything
brief Run with sudo when stuck.
permissions full-auto

team ops
member runner 2
place 1: runner.2
)", "fallback");
    REQUIRE(m.teams.size() == 2);
    const TeamSpec & r = m.teams[0];
    CHECK(r.name == "research-team");
    CHECK(r.brief == "Shared goal before any team line.\nAnswer from the sources.\nWrite REPORT.md.");
    CHECK(r.rule_files.size() == 1 && r.rule_files[0] == "SPEC.md#House rules");
    CHECK(r.rules.size() == 1);
    REQUIRE(r.members.size() == 4);
    CHECK(r.members[0].id == "editor" && r.members[0].role == "lead" && r.members[0].skills.size() == 1);
    CHECK(r.members[1].id == "reader" && r.members[1].count == 3);
    CHECK(r.members[2].model == "cpu" && r.members[2].skills.size() == 2);
    CHECK(r.members[3].role == "reviewer");
    CHECK(r.edges.size() == 2);
    CHECK(r.edges[0].kind == "observe" && r.edges[1].from == "editor" && r.edges[1].to == "triage");
    REQUIRE(r.place.size() == 2);
    CHECK(r.place[0].size() == 1 && r.place[0][0] == "editor");
    CHECK(r.place[1].size() == 4 && r.place[1][0] == "reader.1" && r.place[1][3] == "triage");
    CHECK(validate_team_spec(r, 16, 0, 2).empty());
    auto has = [&](const std::string & w) {
        for (auto & x : m.warnings)
            if (x.find(w) != std::string::npos) return true;
        return false;
    };
    CHECK(has("tools=shell"));
    CHECK(has("already exists"));
    CHECK(has("ghost"));
    CHECK(has("nobody"));
    CHECK(has("frobnicate"));
    CHECK(has("cannot change permissions"));
    CHECK(has("unknown keyword 'permissions'"));
    CHECK(r.brief.find("sudo") == std::string::npos);
    const TeamSpec & o = m.teams[1];
    CHECK(o.name == "ops" && o.members.size() == 1 && o.members[0].count == 2);
    CHECK(o.place.size() == 1 && o.place[0][0] == "runner.2");

    // canonical text round-trips and carries no warnings
    std::string canon = team_map_text(r);
    CHECK(canon.find("team research-team\n") == 0);
    CHECK(canon.find("member reader x3\n") != std::string::npos);
    CHECK(canon.find("member triage x") == std::string::npos);
    CHECK(canon.find("link reader -> reader observe\n") != std::string::npos);
    CHECK(canon.find("place 2: reader.1 reader.2 reader.3 triage\n") != std::string::npos);
    TeamMap again = parse_team_map(canon);
    CHECK(again.warnings.empty());
    REQUIRE(again.teams.size() == 1);
    CHECK(team_map_text(again.teams[0]) == canon);

    // a bare file with no team line takes the default name; JSON keeps place
    TeamMap bare = parse_team_map("member a\nmember b x2\n", "My Map");
    CHECK(bare.teams.size() == 1 && bare.teams[0].name == "my-map");
    TeamSpec js;
    CHECK(team_spec_from_json(team_spec_to_json(r), js).empty() && js.place == r.place);
    TeamSpec badplace = r;
    badplace.place.push_back({"ghost.9"});
    CHECK(!validate_team_spec(badplace, 16, 0, 2).empty());
    // a lead line never makes copies or cpu leads
    TeamMap lead = parse_team_map("team t\nlead boss x3 cpu\nlead second\n");
    CHECK(lead.teams[0].members[0].count == 1 && lead.teams[0].members[0].model == "gpu");
    CHECK(lead.teams[0].members[1].role == "worker");
}

static void test_save() {
    TeamSpec r = research();
    r.brief = "Answer the question.\nWrite REPORT.md.";
    r.rules = {"Quote numbers exactly."};
    TeamSaveInfo info;
    info.date = "2026-11-02";
    info.session = "s-abc";
    info.note = "all copies done \xE2\x80\x94 passed checks\nsecond line";
    info.agents = 5;
    info.tokens = 12345;
    info.wall_s = 61.4;
    info.failed = 1;
    std::string text = team_save_text(r, info);
    CHECK(text.rfind("# Answer the question.\n", 0) == 0);
    CHECK(text.find("# saved: 2026-11-02 session s-abc\n") != std::string::npos);
    CHECK(text.find("# outcome: all copies done - passed checks second line\n") != std::string::npos);
    CHECK(text.find("# stats: 5 agents, 12345 tokens, wall 61 s, 1 failed copies\n") != std::string::npos);
    CHECK(text.find("\xE2\x80\x94") == std::string::npos);
    // round trip: parse(serialize(team)) is the team
    TeamMap m = parse_team_map(text);
    CHECK(m.warnings.empty());
    REQUIRE(m.teams.size() == 1);
    CHECK(team_spec_to_json(m.teams[0]).dump() == team_spec_to_json(r).dump());
    CHECK(team_map_text(m.teams[0]) == team_map_text(r));

    CHECK(team_save_name_ok("research-2"));
    CHECK(!team_save_name_ok("") && !team_save_name_ok("Research") && !team_save_name_ok("a_b") &&
          !team_save_name_ok("../x") && !team_save_name_ok("a b") && !team_save_name_ok(std::string(41, 'a')));

    std::string dir = std::string(JUDAS_TEST_TMP) + "/judas-team-save-" + std::to_string(getpid()) + "/teams";
    std::filesystem::remove_all(dir);
    CHECK(!team_save_file(dir, "../evil", text, true).empty());
    CHECK(team_saved_list(dir).empty());
    CHECK(team_save_file(dir, "research", text, false).empty());       // creates the dir
    CHECK(!team_save_file(dir, "research", "x", false).empty());        // no silent overwrite
    CHECK(std::filesystem::exists(dir + "/research.map") && !std::filesystem::exists(dir + "/research.map.bak"));
    std::string second = team_save_text(r, [&] { auto i = info; i.note = "second run"; return i; }());
    CHECK(team_save_file(dir, "research", second, true).empty());
    CHECK(std::filesystem::exists(dir + "/research.map.bak"));
    CHECK(team_save_file(dir, "other", team_save_text(r, TeamSaveInfo{}), false).empty());
    auto l = team_saved_list(dir);
    REQUIRE(l.size() == 2);                                              // .bak is not listed
    CHECK(l[0].name == "other" && l[0].outcome == "not recorded");
    CHECK(l[1].name == "research" && l[1].title == "Answer the question." && l[1].date == "2026-11-02" &&
          l[1].outcome == "second run");
    std::filesystem::remove_all(std::filesystem::path(dir).parent_path());
}

int main() {
    test_save();
    test_map();
    test_parse();
    test_validate();
    test_copies_and_edges();
    test_rule_text();
    test_block();
    test_status();
    TEST_MAIN_END();
}
