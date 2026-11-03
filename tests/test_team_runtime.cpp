// Swarm teams runtime: a scripted fake engine plays the session lead, a team lead, two readers and a cpu member
// (on an injected remote engine). Checks the tools, edges, the pinned brief and rules, the live view and events.
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "agent/agent.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static ChatResult call(const std::vector<std::pair<std::string, json>> & calls) {
    ChatResult r;
    r.message.role = "assistant";
    static std::atomic<int> n{0};
    for (auto & [name, args] : calls) r.message.tool_calls.push_back({"c" + std::to_string(++n), name, args.dump()});
    r.finish_reason = "tool_calls";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    r.generate_s = 0.1;
    return r;
}
static ChatResult say(const std::string & t) {
    ChatResult r;
    r.message.role = "assistant";
    r.message.content = t;
    r.finish_reason = "stop";
    r.prompt_tokens = 10;
    r.completion_tokens = 5;
    return r;
}
static int assistant_turns(const ChatRequest & req) {
    int n = 0;
    for (auto & m : req.messages) n += m.role == "assistant";
    return n;
}
static std::string all_text(const ChatRequest & req) {
    std::string s;
    for (auto & m : req.messages) s += m.role + ": " + m.content + "\n";
    return s;
}

class ScriptEngine : public Engine {
public:
    std::mutex mu;
    std::vector<std::string> worker_prompts;   // system text a reader saw
    std::string lead_text, team_lead_text;     // everything the lead / team lead saw at its last call
    std::vector<std::string> chat_tools;       // tool names offered to a lead in a chat session
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on, const std::atomic<bool> *) override {
        if (on) on({"", "thinking"});
        int k = assistant_turns(req);
        std::lock_guard lk(mu);
        if (req.tag.rfind("lead ", 0) == 0) {
            lead_text = all_text(req);
            bool chat_session = all_text(req).find("plain chat please") != std::string::npos;
            if (chat_session) {
                for (auto & t : req.tools) chat_tools.push_back(t.name);
                return say("hi");
            }
            if (k == 0)
                return call({{"team_create", {{"map", "team t\nbrief Brief B.\nrule Rule R.\nrules SPEC.md#House rules\n"
                                                              "lead ed\nmember rd x2\nmember cp cpu\nmystery line\n"
                                                              "place 1: ed\nplace 2: rd cp\n"}}}});
            if (k == 1)   // direct to a member past the team lead: refused; then the team lead
                return call({{"team_assign", {{"member", "rd.1"}, {"task", "x"}}},
                             {"team_assign", {{"member", "ed"}, {"task", "Read every chapter."}}}});
            if (k == 2) return call({{"team_wait", json::object()}});
            return call({{"done", {{"summary", "final: merged"}}}});
        }
        if (req.tag.rfind("team-lead ", 0) == 0) {
            team_lead_text = all_text(req);
            if (k == 0)
                return call({{"team_assign", {{"member", "rd"}, {"task", "chapter one"}}},
                             {"team_assign", {{"member", "rd"}, {"task", "chapter two"}}},
                             {"team_assign", {{"member", "cp"}, {"task", "triage"}}}});
            if (k == 1) return call({{"team_wait", json::object()}});
            if (k == 2) return call({{"team_message", {{"to", "lead"}, {"message", "nearly there"}}},
                                     {"team_read", {{"agent", "rd.2"}}}});
            return call({{"done", {{"summary", "team report"}}}});
        }
        // a reader
        worker_prompts.push_back(all_text(req));
        if (k == 0) return call({{"team_read", {{"agent", "rd.2"}}}, {"team_message", {{"to", "lead"}, {"message", "skip"}}}});
        return say("reader report");
    }
    EngineState state() const override {
        EngineState s;
        s.model = "fake";
        s.loaded = true;
        s.max_agents = 4;
        s.ctx_total = 32000;
        return s;
    }
    std::string model_name() const override { return "fake"; }
    int count_tokens(const std::string & t) const override { return (int) t.size() / 4; }
};

class FakeRemote : public Engine {
public:
    std::atomic<int> calls{0};
    std::vector<std::string> tools;
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> &, const std::atomic<bool> *) override {
        calls++;
        tools.clear();
        for (auto & t : req.tools) tools.push_back(t.name);
        return say("cpu triage done");
    }
    EngineState state() const override {
        EngineState s;
        s.model = "small";
        s.loaded = true;
        s.max_agents = 8;
        s.ctx_total = 64000;
        return s;
    }
    std::string model_name() const override { return "small"; }
    int count_tokens(const std::string & t) const override { return (int) t.size() / 4; }
};

static void spit(const fs::path & p, const std::string & t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << t;
}

int main() {
    fs::path base = fs::path(JUDAS_TEST_TMP) / ("team-runtime-" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::path wd = base / "wd";
    spit(wd / "SPEC.md", "# Spec\n\n## House rules (apply to every function)\n- A: strip input\n\n## Module 1\nbody\n");

    Config cfg;
    cfg.data_dir = (base / "data").string();
    cfg.agents.role_dirs = {};
    cfg.agents.max_depth = 2;
    cfg.skills.paths = {(base / "skills").string()};
    cfg.skills.track_usage = false;
    cfg.memory.path = (base / "SHARED.md").string();
    cfg.mcp.enabled = false;
    cfg.learning.review = false;
    cfg.swarm.enabled = true;
    cfg.permissions.mode = "full-auto";
    cfg.permissions.allow_paths = {base.string()};
    cfg.teams.house_rules = {"Config rule C."};

    ScriptEngine engine;
    Store store((base / "sessions.db").string());
    Journal journal(base / "snap");
    AgentManager agents(cfg, engine, store, journal);
    auto remote = std::make_shared<FakeRemote>();
    FakeRemote * rp = remote.get();
    agents.set_remote_engine_factory([rp](const std::string &) {
        struct Proxy : Engine {   // the manager owns what the factory returns; the test keeps the real one
            FakeRemote * r;
            explicit Proxy(FakeRemote * x) : r(x) {}
            ChatResult chat(const ChatRequest & q, const std::function<bool(const ChatDelta &)> & d, const std::atomic<bool> * c) override { return r->chat(q, d, c); }
            EngineState state() const override { return r->state(); }
            std::string model_name() const override { return r->model_name(); }
            int count_tokens(const std::string & t) const override { return r->count_tokens(t); }
        };
        return std::unique_ptr<Engine>(new Proxy(rp));
    });

    std::mutex ev_mu;
    std::vector<AgentEvent> events;
    agents.add_listener([&](const AgentEvent & e) {
        std::lock_guard lk(ev_mu);
        events.push_back(e);
    });

    auto sw = store.create_session("swarm", wd.string(), "fake", "swarm");
    agents.submit(sw.id, "Summarize the book. USER-REQUEST-MARK", wd.string());
    std::string answer = agents.wait_session(sw.id);
    CHECK(answer == "final: merged");

    // the direct assignment past the team lead was refused with a pointer to the team lead
    CHECK(engine.lead_text.find("give the work to the team lead ed") != std::string::npos);
    // the session lead got the team lead's report through team_wait
    CHECK(engine.lead_text.find("team report") != std::string::npos);
    // members' prompts carry the pinned brief, rules (config, spec, verbatim file section) and the user's request
    REQUIRE(engine.worker_prompts.size() >= 2);
    const std::string & wp = engine.worker_prompts.front();
    CHECK(wp.find("[team t] You are rd.") != std::string::npos);
    CHECK(wp.find("Brief B.") != std::string::npos);
    CHECK(wp.find("- Config rule C.") != std::string::npos);
    CHECK(wp.find("- Rule R.") != std::string::npos);
    CHECK(wp.find("- A: strip input") != std::string::npos);
    CHECK(wp.find("Module 1") == std::string::npos);
    CHECK(wp.find("USER-REQUEST-MARK") != std::string::npos);
    // readers have no observe edge to each other and may only message their team lead
    CHECK(engine.team_lead_text.find("reader report") != std::string::npos);
    // the cpu member ran on the remote engine with read-only and team tools only
    CHECK(rp->calls.load() == 1);
    for (auto & t : rp->tools)
        CHECK(t != "shell" && t != "write_file" && t != "edit_file" && t != "team_assign" && t != "spawn_agent");
    bool has_done = false;
    for (auto & t : rp->tools) has_done |= t == "done";
    CHECK(has_done);

    // the map came back canonical, with the skipped line reported to the lead
    CHECK(engine.lead_text.find("member rd x2") != std::string::npos);
    CHECK(engine.lead_text.find("unknown keyword 'mystery'") != std::string::npos);

    // the live view
    json v = agents.team_view(sw.id);
    REQUIRE(v["teams"].size() == 1);
    CHECK(v["teams"][0]["name"] == "t");
    CHECK(v["teams"][0]["lead_member"] == "ed");
    CHECK(v["teams"][0]["copies"].size() == 4);
    int started = 0;
    for (auto & c : v["teams"][0]["copies"]) started += !c["agent"].get<std::string>().empty();
    CHECK(started == 4);
    CHECK(v["lead"].is_object() && v["lead"]["activity"] == "done");
    REQUIRE(v["teams"][0]["place"].size() == 2);
    CHECK(v["teams"][0]["place"][1].size() == 3 && v["teams"][0]["place"][1][2] == "cp");
    CHECK(v["teams"][0]["map"].get<std::string>().find("place 2: rd.1 rd.2 cp") != std::string::npos);
    CHECK(v["teams"][0]["warnings"].size() == 1);
    CHECK(v["lead"].contains("ctx_pct") && v["lead"]["model_name"] == "fake");
    int cpu = 0, done = 0;
    for (auto & a : v["agents"]) {
        cpu += a["model"] == "cpu";
        done += a["activity"] == "done";
        if (a["copy"] == "cp") CHECK(a["last_output"] == "cpu triage done");
    }
    CHECK(cpu == 1);
    CHECK(done == 5);

    // events: activity changes, team actions, spawned with team data
    {
        std::lock_guard lk(ev_mu);
        int act = 0, created = 0, assigned = 0, spawned_team = 0, msgs = 0, reports = 0, from_lead = 0;
        for (auto & e : events) {
            act += e.type == "activity";
            if (e.type == "team") {
                std::string a = e.data.value("action", "");
                created += a == "created";
                assigned += a == "assigned";
                msgs += a == "message";
                reports += a == "report";
                if (a == "assigned" && e.data.value("from", "") == "lead" && e.data.value("to", "") == "ed") from_lead++;
                if (a == "report" && e.data.value("from", "") == "rd.1") CHECK(e.data.value("to", "") == "ed");
                if (a == "report" && e.data.value("from", "") == "ed") CHECK(e.data.value("to", "") == "lead");
            }
            spawned_team += e.type == "spawned" && e.data.contains("team");
        }
        CHECK(act > 10);
        CHECK(created == 1);
        CHECK(assigned == 4);
        CHECK(spawned_team == 4);
        CHECK(msgs >= 1);
        CHECK(reports == 4);
        CHECK(from_lead == 1);
    }

    // a chat session never sees the team tools
    auto ch = store.create_session("chat", wd.string(), "fake", "chat");
    agents.submit(ch.id, "plain chat please", wd.string());
    agents.wait_session(ch.id);
    REQUIRE(!engine.chat_tools.empty());
    for (auto & t : engine.chat_tools) CHECK(t.rfind("team_", 0) != 0);

    // per-agent controls refuse unknown agents and other sessions
    CHECK(!agents.cancel_agent(sw.id, "nope"));
    CHECK(!agents.pause_agent(ch.id, v["lead"]["id"].get<std::string>()));
    CHECK(!agents.resume_agent(sw.id, v["lead"]["id"].get<std::string>()));   // done, not paused

    fs::remove_all(base);
    TEST_MAIN_END();
}
