// Approvals over HTTP: a scripted engine asks for tools that need the user's yes; the test answers through
// GET/POST /api/sessions/<id>/approvals. Covers approve, decline, 404/409/400, cancel, timeout, full-auto,
// the safety floor (refused before any approval is raised) and strict approvals (mach12 planner tools).
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "agent/agent.h"
#include "cpp-httplib/httplib.h"
#include "server/server.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static ChatResult call1(const std::string & name, const json & args) {
    ChatResult r;
    r.message.role = "assistant";
    static std::atomic<int> n{0};
    r.message.tool_calls.push_back({"c" + std::to_string(++n), name, args.dump()});
    r.finish_reason = "tool_calls";
    r.prompt_tokens = 5;
    r.completion_tokens = 2;
    return r;
}
static ChatResult say(const std::string & t) {
    ChatResult r;
    r.message.role = "assistant";
    r.message.content = t;
    r.finish_reason = "stop";
    r.prompt_tokens = 5;
    r.completion_tokens = 2;
    return r;
}

static std::string g_dir;   // files the scripted tools touch

// Reads the user's marker from the first user message; step 0 calls the tool, later steps answer.
class ScriptEngine : public Engine {
public:
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> &, const std::atomic<bool> *) override {
        std::string user;
        int turns = 0;
        for (auto & m : req.messages) {
            if (m.role == "user" && user.empty()) user = m.content;
            turns += m.role == "assistant";
        }
        if (turns > 0) return say("finished");
        auto has = [&](const char * k) { return user.find(k) != std::string::npos; };
        if (has("WRITE")) return call1("write_file", {{"path", g_dir + "/" + (has("WRITE2") ? "w2.txt" : has("WRITE3") ? "w3.txt" : "w.txt")}, {"content", "one\ntwo\n"}});
        if (has("EDIT")) return call1("edit_file", {{"path", g_dir + "/e.txt"}, {"old_string", "beta"}, {"new_string", "BETA\ngamma"}});
        if (has("SUDO")) return call1("shell", {{"command", "sudo ls"}});
        if (has("PLAN")) return call1("mach12_add", {{"date", "2026-10-28"}, {"start", has("PLAN2") ? "18:00" : "15:00"}, {"end", has("PLAN2") ? "19:00" : "16:00"}, {"title", has("PLAN2") ? "Declined block" : "Approved block"}});
        return say("nothing");
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

static std::string slurp(const fs::path & p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static bool has(const std::string & s, const std::string & n) { return s.find(n) != std::string::npos; }

int main() {
    fs::path base = fs::path(JUDAS_TEST_TMP) / ("approvals-api-" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base / "wd");
    g_dir = (base / "wd").string();
    std::ofstream(base / "wd" / "e.txt") << "alpha\nbeta\n";

    Config cfg;
    cfg.data_dir = (base / "data").string();
    cfg.agents.role_dirs = {};
    cfg.agents.auto_resume_delay_s = 0;
    cfg.skills.paths = {(base / "skills").string()};
    cfg.skills.track_usage = false;
    cfg.memory.path = (base / "SHARED.md").string();
    cfg.mcp.enabled = false;
    cfg.learning.review = false;
    cfg.permissions.mode = "suggest";
    cfg.permissions.allow_paths = {base.string()};
    cfg.server.port = 18400 + getpid() % 500;
#ifdef JUDAS_MACH12
    fs::path data = base / "m12";
    fs::copy(fs::path(JUDAS_TESTS_DIR) / "fixtures" / "mach12", data, fs::copy_options::recursive);
    fs::path fake = base / "merge-fake";
    std::ofstream(fake) << "#!/usr/bin/env python3\n"
        "import json, os, sys\n"
        "d = os.environ['MACH12_DATA']; args = sys.argv[1:]; inp = json.loads(sys.stdin.read())['events']\n"
        "p = d + '/events.json'\n"
        "cur = json.load(open(p))['events'] if os.path.exists(p) else []\n"
        "out = json.dumps({'events': cur + inp}, indent=2) + '\\n'\n"
        "if '--dry-run' in args: print(out, end='')\n"
        "else: open(p, 'w').write(out)\n";
    fs::permissions(fake, fs::perms::owner_all);
    cfg.mach12.enabled = true;
    cfg.mach12.data_dir = data.string();
    cfg.mach12.merge_cmd = fake.string();
#endif

    ScriptEngine engine;
    Store store((base / "sessions.db").string());
    Journal journal(base / "snap");
    AgentManager agents(cfg, engine, store, journal);
    agents.enable_remote_approvals();
    Server server(cfg, engine, agents, store, journal);
    std::thread th([&] { server.run(); });

    httplib::Client cli("127.0.0.1", cfg.server.port);
    cli.set_read_timeout(20);
    bool up = false;
    for (int i = 0; i < 100 && !up; i++) {
        auto r = cli.Get("/health");
        up = r && r->status == 200;
        if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(up);

    auto start = [&](const std::string & text) {
        auto r = cli.Post("/api/sessions/new/messages", json{{"content", text}, {"cwd", g_dir}}.dump(), "application/json");
        REQUIRE(r && r->status == 200);
        { json j = json::parse(r->body); return j["session"].is_string() ? j["session"].get<std::string>() : j["session"]["id"].get<std::string>(); }
    };
    auto pending = [&](const std::string & sid) {
        auto r = cli.Get("/api/sessions/" + sid + "/approvals");
        REQUIRE(r && r->status == 200);
        return json::parse(r->body)["approvals"];
    };
    auto wait_pending = [&](const std::string & sid, size_t n, int ms = 10000) {
        for (int i = 0; i < ms / 50; i++) {
            if (pending(sid).size() == n) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    };
    auto answer = [&](const std::string & sid, const std::string & aid, const std::string & body) {
        auto r = cli.Post("/api/sessions/" + sid + "/approvals/" + aid, body, "application/json");
        REQUIRE(r);
        return r;
    };
    auto finished = [&](const std::string & sid) {
        agents.wait_session(sid);
        return agents.turn_info(sid).status;
    };
    auto replay = [&](const std::string & sid) {
        auto r = cli.Get("/api/sessions/" + sid + "/events?since=0");
        return r ? r->body : std::string();
    };

    // unknown session
    {
        auto r = cli.Get("/api/sessions/nope/approvals");
        CHECK(r && r->status == 404);
    }

    // approve a write: pending with the diff text, the file only appears after the yes, one answer only
    {
        std::string sid = start("WRITE please");
        REQUIRE(wait_pending(sid, 1));
        json a = pending(sid)[0];
        std::string aid = a["id"];
        CHECK(a["tool"] == "write_file" && a["strict"] == false && a["created_ms"].get<long long>() > 0);
        CHECK(a["agent"].is_string() && a["role"] == "lead");
        std::string what = a["what"];
        CHECK(has(what, "write_file ") && has(what, "/w.txt") && has(what, "\n+one\n+two"));
        CHECK(!fs::exists(base / "wd" / "w.txt"));
        CHECK(answer(sid, aid, "not json")->status == 400);
        CHECK(answer(sid, aid, R"({"approve":"yes"})")->status == 400);
        CHECK(answer(sid, "ap-nope", R"({"approve":true})")->status == 404);
        CHECK(answer("other-session", aid, R"({"approve":true})")->status == 404);
        auto ok = answer(sid, aid, R"({"approve":true})");
        CHECK(ok->status == 200 && json::parse(ok->body)["approved"] == true);
        CHECK(answer(sid, aid, R"({"approve":false})")->status == 409);
        CHECK(finished(sid) == "done");
        CHECK(slurp(base / "wd" / "w.txt") == "one\ntwo\n");
        CHECK(pending(sid).empty());
        std::string ev = replay(sid);
        CHECK(has(ev, "approval_request") && has(ev, "approval_resolved") && has(ev, R"("approved":true)") && has(ev, R"("by":"user")"));
    }

    // decline an edit: the file is untouched, the tool result says declined, the turn still finishes
    {
        std::string sid = start("EDIT please");
        REQUIRE(wait_pending(sid, 1));
        json a = pending(sid)[0];
        CHECK(a["tool"] == "edit_file" && has(a["what"].get<std::string>(), "\n-beta\n+BETA\n+gamma"));
        auto r = answer(sid, a["id"], R"({"approve":false})");
        CHECK(r->status == 200 && json::parse(r->body)["approved"] == false);
        CHECK(finished(sid) == "done");
        CHECK(slurp(base / "wd" / "e.txt") == "alpha\nbeta\n");
        std::string ev = replay(sid);
        CHECK(has(ev, "was declined") && has(ev, R"("approved":false)"));
    }

    // cancelling the session declines what is pending
    {
        std::string sid = start("WRITE2 cancel");
        REQUIRE(wait_pending(sid, 1));
        auto c = cli.Post("/api/sessions/" + sid + "/cancel", "{}", "application/json");
        CHECK(c && c->status == 200);
        CHECK(wait_pending(sid, 0, 5000));
        agents.wait_session(sid);
        CHECK(!fs::exists(base / "wd" / "w2.txt"));
        CHECK(has(replay(sid), R"("by":"cancelled")"));
    }

    // the timeout declines too, so an unattended session never hangs for good
    {
        agents.set_approval_timeout(1);
        std::string sid = start("WRITE3 timeout");
        REQUIRE(wait_pending(sid, 1));
        CHECK(wait_pending(sid, 0, 6000));
        agents.wait_session(sid);
        CHECK(!fs::exists(base / "wd" / "w3.txt"));
        CHECK(has(replay(sid), R"("by":"timeout")"));
        agents.set_approval_timeout(600);
    }

    // the safety floor is refused before any approval is raised
    {
        std::string sid = start("SUDO now");
        agents.wait_session(sid);
        CHECK(pending(sid).empty());
        std::string ev = replay(sid);
        CHECK(!has(ev, "approval_request"));
    }

    // full-auto never raises an ordinary approval
    {
        cfg.permissions.mode = "full-auto";
        fs::remove(base / "wd" / "w.txt");
        std::string sid = start("WRITE auto");
        CHECK(finished(sid) == "done");
        CHECK(fs::exists(base / "wd" / "w.txt"));
        CHECK(!has(replay(sid), "approval_request"));
    }

#ifdef JUDAS_MACH12
    // strict approvals (planner tools) are asked even in full-auto, and answered through the same API
    {
        cfg.permissions.mode = "full-auto";
        std::string before = slurp(data / "events.json");
        std::string sid = start("PLAN it");
        REQUIRE(wait_pending(sid, 1));
        json a = pending(sid)[0];
        CHECK(a["strict"] == true && a["tool"] == "mach12_add");
        CHECK(has(a["what"].get<std::string>(), "dry run") && has(a["what"].get<std::string>(), "Approved block"));
        CHECK(slurp(data / "events.json") == before);   // nothing applied before the yes
        CHECK(answer(sid, a["id"], R"({"approve":true})")->status == 200);
        CHECK(finished(sid) == "done");
        CHECK(has(slurp(data / "events.json"), "Approved block"));
        CHECK(has(replay(sid), R"("strict":true)"));

        // declined: still nothing applied
        std::string sid2 = start("PLAN2 again");
        REQUIRE(wait_pending(sid2, 1));
        CHECK(answer(sid2, pending(sid2)[0]["id"], R"({"approve":false})")->status == 200);
        CHECK(finished(sid2) == "done");
        std::string after = slurp(data / "events.json");
        CHECK(has(after, "Approved block") && !has(after, "Declined block"));
    }
#endif

    server.stop();
    th.join();
    TEST_MAIN_END();
}
