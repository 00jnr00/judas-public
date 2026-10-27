// Server tests: FakeEngine behind the real Store, Journal and AgentManager, hit over HTTP.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "agent/autoresume.h"
#include "cpp-httplib/httplib.h"
#include "server/server.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

class FakeEngine : public Engine {
public:
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on_delta,
                    const std::atomic<bool> * cancel) override {
        // a user message containing "slow" keeps the turn running for ~3 s (or until cancelled)
        bool slow = false, loop = false;
        for (auto & m : req.messages)
            if (m.role == "user" && m.content.find("slow") != std::string::npos) slow = true;
        for (auto & m : req.messages)
            if (m.role == "user" && m.content.find("loopme") != std::string::npos) loop = true;
        if (loop) {   // the same failing call forever: the loop guard pauses the turn
            ChatResult lr;
            lr.message.role = "assistant";
            lr.message.tool_calls.push_back({"c" + std::to_string(++calls_), "read_file", R"({"path":"/nonexistent/loop"})"});
            lr.finish_reason = "tool_calls";
            lr.prompt_tokens = 5;
            lr.completion_tokens = 2;
            return lr;
        }
        if (slow)
            for (int i = 0; i < 60 && !(cancel && cancel->load()); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ChatResult r;
        r.message.role = "assistant";
        r.message.content = "Hello there";
        r.finish_reason = "stop";
        r.prompt_tokens = 7;
        r.completion_tokens = 3;
        if (on_delta) {
            on_delta({"", "thinking"});
            on_delta({"Hello", ""});
            on_delta({" there", ""});
        }
        return r;
    }
    EngineState state() const override {
        EngineState s;
        s.model = "fake";
        s.loaded = true;
        s.max_agents = 4;
        s.seqs.push_back({0, "agent a-1", "idle", 12, 1});
        s.ctx_total = 1000; s.ctx_active = 300; s.ctx_free = 450; s.kv_reserve = 250; s.preemptions = 3;
        s.queue.push_back({"agent a-2", 1, 120, 2.5, 1});
        return s;
    }
    std::string model_name() const override { return "fake"; }
    int count_tokens(const std::string & t) const override { return (int)t.size() / 4; }

private:
    std::atomic<int> calls_{0};
};

int main() {
    setenv("JUDAS_AUTO_RESUME_TICK_S", "1", 1);
    fs::path base = JUDAS_TEST_TMP "/judas-server-test-" + std::to_string(getpid());
    fs::remove_all(base);
    fs::create_directories(base);

    Config cfg;
    cfg.agents.role_dirs = {};
    cfg.agents.auto_resume_delay_s = 0;   // auto-resume is off globally; the paused-turn test turns it on per session
    cfg.server.port = 18079 + getpid() % 1000;
    cfg.server.cors_origins = {"http://hub.test:8010"};
    FakeEngine engine;
    Store store((base / "sessions.db").string());
    Journal journal(base / "snap");
    AgentManager agents(cfg, engine, store, journal);
    Server server(cfg, engine, agents, store, journal);

    bool ran = false;
    std::thread th([&] { ran = server.run(); });

    httplib::Client cli("127.0.0.1", cfg.server.port);
    cli.set_read_timeout(20);
    bool up = false;
    for (int i = 0; i < 100 && !up; i++) {
        auto r = cli.Get("/health");
        up = r && r->status == 200;
        if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(up);

    {
        auto r = cli.Get("/health");
        auto j = json::parse(r->body);
        CHECK(j["status"] == "ok");
        CHECK(j["model"] == "fake");
    }
    {
        auto r = cli.Get("/state");
        REQUIRE(r && r->status == 200);
        auto j = json::parse(r->body);
        CHECK(j["model"] == "fake");
        CHECK(j["seqs"].size() == 1);
        CHECK(j.contains("sessions"));
        CHECK(j["version"].is_string());
    }
    for (const char * path : {"/state", "/api/state"}) {   // KV admission fields
        auto r = cli.Get(path);
        REQUIRE(r && r->status == 200);
        auto j = json::parse(r->body);
        CHECK(j["ctx_active"] == 300 && j["ctx_free"] == 450 && j["kv_reserve"] == 250 && j["preemptions"] == 3);
        REQUIRE(j["queue"].size() == 1);
        CHECK(j["queue"][0]["tag"] == "agent a-2" && j["queue"][0]["prompt_tokens"] == 120);
        CHECK(j["queue"][0]["preempted"] == 1 && j["queue"][0]["waited_s"] == 2.5);
    }
    {
        auto r = cli.Get("/v1/models");
        auto j = json::parse(r->body);
        CHECK(j["data"].size() == 2);
        CHECK(j["data"][0]["id"] == "fake");
        CHECK(r->get_header_value("Access-Control-Allow-Origin").empty());   // no wildcard CORS
        auto ok = cli.Get("/v1/models", httplib::Headers{{"Origin", "http://hub.test:8010"}});
        CHECK(ok && ok->get_header_value("Access-Control-Allow-Origin") == "http://hub.test:8010");
        auto other = cli.Get("/v1/models", httplib::Headers{{"Origin", "https://evil.example"}});
        CHECK(other && other->status == 200 && other->get_header_value("Access-Control-Allow-Origin").empty());
        // a page elsewhere cannot write, even with a simple form-style POST that skips the preflight
        auto post = cli.Post("/api/learn", httplib::Headers{{"Origin", "https://evil.example"}}, "{}", "text/plain");
        CHECK(post && post->status == 403);
        // DNS rebinding: a foreign Host name is refused
        auto reb = cli.Get("/v1/models", httplib::Headers{{"Host", "evil.example:8079"}});
        CHECK(reb && reb->status == 403);
    }
    {
        auto r = cli.Post("/v1/chat/completions",
                          R"({"model":"main","messages":[{"role":"user","content":"hi"}]})", "application/json");
        REQUIRE(r && r->status == 200);
        auto j = json::parse(r->body);
        CHECK(j["object"] == "chat.completion");
        CHECK(j["choices"][0]["message"]["content"] == "Hello there");
        CHECK(j["choices"][0]["finish_reason"] == "stop");
        CHECK(j["usage"]["total_tokens"] == 10);
    }
    {
        auto r = cli.Post("/v1/chat/completions", "not json", "application/json");
        CHECK(r && r->status == 400);
        CHECK(json::parse(r->body).contains("error"));
    }
    {
        auto r = cli.Post("/v1/chat/completions",
                          R"({"messages":[{"role":"user","content":"hi"}],"stream":true,"stream_options":{"include_usage":true}})",
                          "application/json");
        REQUIRE(r && r->status == 200);
        std::string content, reasoning, role, finish;
        bool usage = false, done = false;
        size_t pos = 0;
        while ((pos = r->body.find("data: ", pos)) != std::string::npos) {
            size_t end = r->body.find("\n\n", pos);
            std::string d = r->body.substr(pos + 6, end - pos - 6);
            pos = end;
            if (d == "[DONE]") { done = true; continue; }
            auto j = json::parse(d);
            CHECK(j["object"] == "chat.completion.chunk");
            if (j.contains("usage")) usage = true;
            if (j["choices"].empty()) continue;
            auto & c = j["choices"][0];
            auto & dl = c["delta"];
            if (dl.contains("role")) role = dl["role"];
            if (dl.contains("content")) content += dl["content"].get<std::string>();
            if (dl.contains("reasoning_content")) reasoning += dl["reasoning_content"].get<std::string>();
            if (c["finish_reason"].is_string()) finish = c["finish_reason"];
        }
        CHECK(role == "assistant");
        CHECK(content == "Hello there");
        CHECK(reasoning == "thinking");
        CHECK(finish == "stop");
        CHECK(usage);
        CHECK(done);
    }
    {
        auto r = cli.Post("/api/sessions/new/messages", R"({"content":"do a thing","cwd":"/tmp"})",
                          "application/json");
        REQUIRE(r && r->status == 200);
        std::string sid = json::parse(r->body)["session"];
        CHECK(sid.rfind("s-", 0) == 0);
        std::string seen;
        auto ev = cli.Get("/api/sessions/" + sid + "/events", [&](const char * d, size_t n) {
            seen.append(d, n);
            return true;
        });
        REQUIRE(ev && ev->status == 200);
        CHECK(seen.find("\"type\":\"done\"") != std::string::npos);
        auto s = cli.Get("/api/sessions/" + sid);
        CHECK(s && s->status == 200 && json::parse(s->body)["agents"].size() >= 1);
        // A hidden review agent is omitted from the agents list unless ?all=1.
        size_t n_before = json::parse(s->body)["agents"].size();
        store.create_agent(sid, "", "review", "background review");
        auto s2 = cli.Get("/api/sessions/" + sid);
        CHECK(s2 && json::parse(s2->body)["agents"].size() == n_before);
        auto s3 = cli.Get("/api/sessions/" + sid + "?all=1");
        CHECK(s3 && json::parse(s3->body)["agents"].size() == n_before + 1);
        auto l = cli.Get("/api/sessions");
        CHECK(json::parse(l->body)["sessions"].size() == 1);
        auto sn = cli.Get("/api/snapshots?session=" + sid);
        CHECK(sn && sn->status == 200);
    }
    {
        auto o = cli.Options("/v1/chat/completions");
        CHECK(o && o->status == 204);
        auto nf = cli.Get("/api/snapshots/nope/diff");
        CHECK(nf && nf->status == 404);
    }

    // --- session mode, effective cwd, pagination (the UI must not depend on browser storage) ---
    std::string msid;
    {
        auto r = cli.Post("/api/sessions/new/messages", R"({"content":"in code mode","mode":"code"})",
                          "application/json");
        REQUIRE(r && r->status == 200);
        msid = json::parse(r->body)["session"];
        auto g = json::parse(cli.Get("/api/sessions/" + msid)->body);
        CHECK(g["session"]["mode"] == "code");
        CHECK(!g["session"]["cwd"].get<std::string>().empty());   // no cwd sent: the effective one is stored
        auto bad = cli.Post("/api/sessions/new/messages", R"({"content":"x","mode":"nonsense"})", "application/json");
        CHECK(bad && bad->status == 400);

        auto p = cli.Patch("/api/sessions/" + msid, R"({"mode":"cowork"})", "application/json");
        REQUIRE(p && p->status == 200);
        CHECK(json::parse(p->body)["mode"] == "cowork");
        auto pb = cli.Patch("/api/sessions/" + msid, R"({"mode":"x"})", "application/json");
        CHECK(pb && pb->status == 400);
        auto pn = cli.Patch("/api/sessions/nope", R"({"mode":"chat"})", "application/json");
        CHECK(pn && pn->status == 404);
        bool listed = false;
        for (auto & s : json::parse(cli.Get("/api/sessions")->body)["sessions"])
            if (s["id"] == msid) listed = s["mode"] == "cowork";
        CHECK(listed);

        auto cr = cli.Post("/api/sessions", R"({"title":"made","mode":"code","cwd":"/tmp"})", "application/json");
        auto cj = json::parse(cr->body);
        CHECK(cj["mode"] == "code" && cj["cwd"] == "/tmp");
        auto cd = json::parse(cli.Post("/api/sessions", "{}", "application/json")->body);
        CHECK(cd["mode"] == "chat" && !cd["cwd"].get<std::string>().empty());
        auto cbad = cli.Post("/api/sessions", R"({"mode":"zzz"})", "application/json");
        CHECK(cbad && cbad->status == 400);

        // wait for the turn, then page the history
        std::string seen;
        cli.Get("/api/sessions/" + msid + "/events", [&](const char * d, size_t n) { seen.append(d, n); return true; });
        auto full = json::parse(cli.Get("/api/sessions/" + msid)->body);
        size_t n_all = full["messages"].size();
        CHECK(n_all >= 2);
        CHECK(full["total"] == (long long) n_all);
        CHECK(full["more_before"] == false);
        auto page = json::parse(cli.Get("/api/sessions/" + msid + "?limit=1")->body);
        REQUIRE(page["messages"].size() == 1);
        CHECK(page["more_before"] == true);
        CHECK(page["messages"][0]["id"] == full["messages"][n_all - 1]["id"]);
        auto older = json::parse(cli.Get("/api/sessions/" + msid + "?limit=5&before=" +
                                         std::to_string(page["messages"][0]["id"].get<long long>()))->body);
        CHECK(older["messages"].size() == n_all - 1);
        CHECK(older["more_before"] == false);
        auto lim = json::parse(cli.Get("/api/sessions?limit=1")->body);
        CHECK(lim["sessions"].size() == 1);
        CHECK(lim["more"] == true);
    }


    // --- paused turns: status in GET/list/stream, resume route, per-session auto-resume with the stall stop ---
    {
        auto status_of = [&](const std::string & sid) {
            auto g = cli.Get("/api/sessions/" + sid);
            return g && g->status == 200 ? json::parse(g->body)["turn"]["status"].get<std::string>() : std::string("?");
        };
        auto wait_status = [&](const std::string & sid, const std::string & want) {
            for (int i = 0; i < 300 && status_of(sid) != want; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return status_of(sid) == want;
        };
        auto resumes = [&](const std::string & sid) {
            int n = 0;
            for (auto & m : json::parse(cli.Get("/api/sessions/" + sid)->body)["messages"])
                if (m["role"] == "user" && m["body"].dump().find("Resume the task") != std::string::npos) n++;
            return n;
        };
        auto r = cli.Post("/api/sessions/new/messages", R"({"content":"loopme please","cwd":"/tmp"})", "application/json");
        REQUIRE(r && r->status == 200);
        std::string psid = json::parse(r->body)["session"];
        REQUIRE(wait_status(psid, "paused"));
        auto g = json::parse(cli.Get("/api/sessions/" + psid)->body);
        CHECK(g["turn"]["status"] == "paused");
        CHECK(g["turn"]["reason"].get<std::string>().find("looping") != std::string::npos);
        CHECK(g["turn"]["open_items"] == 0);
        CHECK(g["session"]["auto_resume"].is_null());
        bool row = false;
        for (auto & s : json::parse(cli.Get("/api/sessions")->body)["sessions"])
            if (s["id"] == psid) row = s["turn_status"] == "paused" && s["open_items"] == 0 &&
                                       s["turn_reason"].get<std::string>().find("looping") != std::string::npos;
        CHECK(row);
        // a reconnecting client (no follow, no since) gets a synthetic done that says paused
        auto ev = cli.Get("/api/sessions/" + psid + "/events");
        REQUIRE(ev && ev->status == 200);
        CHECK(ev->body.find("\"type\":\"done\"") != std::string::npos);
        CHECK(ev->body.find("\"status\":\"paused\"") != std::string::npos);
        // resume route
        auto nf = cli.Post("/api/sessions/nope/resume", "", "application/json");
        CHECK(nf && nf->status == 404);
        auto conflict = cli.Post("/api/sessions/" + msid + "/resume", "", "application/json");   // a finished, not paused one
        CHECK(conflict && conflict->status == 409);
        auto rs = cli.Post("/api/sessions/" + psid + "/resume", "", "application/json");
        REQUIRE(rs && rs->status == 200);
        CHECK(json::parse(rs->body)["ok"] == true);
        CHECK(resumes(psid) == 1);
        REQUIRE(wait_status(psid, "paused"));   // it loops and pauses again
        // per-session auto-resume: validated, stored, and it stops after 3 resumes that changed nothing
        auto bad = cli.Patch("/api/sessions/" + psid, R"({"auto_resume":"yes"})", "application/json");
        CHECK(bad && bad->status == 400);
        auto on = cli.Patch("/api/sessions/" + psid, R"({"auto_resume":true})", "application/json");
        REQUIRE(on && on->status == 200);
        CHECK(json::parse(on->body)["auto_resume"] == true);
        int before = resumes(psid), last = -1;
        for (int i = 0; i < 40; i++) {   // settle: the count stops growing
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            int now = resumes(psid);
            if (i > 4 && now == last) break;
            last = now;
        }
        CHECK(resumes(psid) - before == kAutoResumeStallLimit);
        REQUIRE(wait_status(psid, "paused"));
        auto off = cli.Patch("/api/sessions/" + psid, R"({"auto_resume":null})", "application/json");
        CHECK(off && json::parse(off->body)["auto_resume"].is_null());
    }

    // --- replayable per-session stream: ids, ?since=, Last-Event-ID, follow keeps the stream open ---
    {
        auto collect = [&](const std::string & path, const httplib::Headers & hd, const std::string & stop_at) {
            httplib::Client c("127.0.0.1", cfg.server.port);
            c.set_read_timeout(20);
            std::vector<std::pair<long long, std::string>> out;   // (id, type) in arrival order
            std::string buf;
            c.Get(path, hd, [&](const char * d, size_t n) {
                buf.append(d, n);
                size_t pos;
                bool stop = false;
                while ((pos = buf.find("\n\n")) != std::string::npos) {
                    std::string frame = buf.substr(0, pos);
                    buf.erase(0, pos + 2);
                    long long id = 0;
                    size_t ip = frame.find("id: ");
                    if (ip != std::string::npos) id = std::atoll(frame.c_str() + ip + 4);
                    size_t dp = frame.find("data: ");
                    if (dp == std::string::npos) continue;
                    auto j = json::parse(frame.substr(dp + 6), nullptr, false);
                    if (!j.is_object()) continue;
                    out.push_back({id, j.value("type", "")});
                    if (j.value("type", "") == stop_at) stop = true;
                }
                return !stop;
            });
            return out;
        };
        // replay everything for the finished turn: ids rise by one
        auto all = collect("/api/sessions/" + msid + "/events?since=0&follow=1", {}, "done");
        REQUIRE(all.size() >= 5);
        for (size_t i = 1; i < all.size(); i++) CHECK(all[i].first == all[i - 1].first + 1);
        CHECK(all.front().second == "spawned");
        CHECK(all.back().second == "done");
        // resume from the middle (Last-Event-ID header): only newer events come
        long long mid = all[1].first;
        auto rest = collect("/api/sessions/" + msid + "/events?follow=1", {{"Last-Event-ID", std::to_string(mid)}}, "done");
        REQUIRE(!rest.empty());
        CHECK(rest.front().first == mid + 1);
        CHECK(rest.back().first == all.back().first);
        // a turn started by another client shows up on an already open follow stream
        std::vector<std::pair<long long, std::string>> live;
        long long head = all.back().first;
        std::thread watcher([&] {
            live = collect("/api/sessions/" + msid + "/events?since=" + std::to_string(head) + "&follow=1", {}, "done");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto again = cli.Post("/api/sessions/" + msid + "/messages", R"({"content":"second turn"})", "application/json");
        CHECK(again && again->status == 200);
        watcher.join();
        REQUIRE(!live.empty());
        CHECK(live.front().second == "spawned");
        CHECK(live.front().first == head + 1);
        CHECK(live.back().second == "done");
        auto g = json::parse(cli.Get("/api/sessions/" + msid)->body);
        CHECK(g["turn"]["running"] == false);   // idle once the turn ended
    }

    // --- a turn in progress: a fresh connection (a refreshed page) replays it from its first event ---
    {
        auto r = cli.Post("/api/sessions/new/messages", R"({"content":"slow one"})", "application/json");
        REQUIRE(r && r->status == 200);
        std::string ssid = json::parse(r->body)["session"];
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto g = json::parse(cli.Get("/api/sessions/" + ssid)->body);
        CHECK(g["turn"]["running"] == true);
        long long floor = g["turn"]["event_floor"];
        std::string seen;
        cli.Get("/api/sessions/" + ssid + "/events?follow=1", [&](const char * d, size_t n) {
            seen.append(d, n);
            return seen.find("\"type\":\"done\"") == std::string::npos;
        });
        CHECK(seen.find("id: " + std::to_string(floor + 1) + "\n") == 0);   // starts at the turn's first event
        CHECK(seen.find("\"type\":\"spawned\"") != std::string::npos);
        CHECK(seen.find("Hello") != std::string::npos);

        // delete while the next turn is still running: cancel, wait for the agent, then delete, no crash
        auto r2 = cli.Post("/api/sessions/" + ssid + "/messages", R"({"content":"slow two"})", "application/json");
        REQUIRE(r2 && r2->status == 200);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto del = cli.Delete("/api/sessions/" + ssid);
        REQUIRE(del && del->status == 200);
        auto gone = cli.Get("/api/sessions/" + ssid);
        CHECK(gone && gone->status == 404);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the cancelled agent must not blow up
        auto h = cli.Get("/health");
        CHECK(h && h->status == 200);
    }

    // --- global feed: session and turn changes without polling ---
    {
        std::string seen;
        cli.Get("/api/events?since=0", [&](const char * d, size_t n) {
            seen.append(d, n);
            return seen.find("session.deleted") == std::string::npos;
        });
        CHECK(seen.find("session.created") != std::string::npos);
        CHECK(seen.find("turn.started") != std::string::npos);
        CHECK(seen.find("turn.finished") != std::string::npos);
        CHECK(seen.find("agent.status") != std::string::npos);
    }

    // --- extra binds never abort the daemon; a bind that fails at start is retried until it works ---
    {
        int port = cfg.server.port + 1000;
        // a plain socket (no SO_REUSE*) holds 127.0.0.2:port, so Judas cannot bind it yet
        int squat = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t) port);
        inet_pton(AF_INET, "127.0.0.2", &sa.sin_addr);
        REQUIRE(bind(squat, (sockaddr *) &sa, sizeof sa) == 0);
        REQUIRE(listen(squat, 4) == 0);
        Config c2 = cfg;
        c2.server.port = port;
        c2.server.extra_binds = {"127.0.0.2", "10.255.255.1"};   // one busy, one not an address of this host
        Server s2(c2, engine, agents, store, journal);
        bool ran2 = false;
        std::thread t2([&] { ran2 = s2.run(); });
        httplib::Client primary("127.0.0.1", port), extra("127.0.0.2", port);
        bool up2 = false;
        for (int i = 0; i < 100 && !up2; i++) {
            auto r = primary.Get("/health");
            up2 = r && r->status == 200 && r->body.find("fake") != std::string::npos;
            if (!up2) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(up2);   // primary is serving although the extras failed
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        {
            httplib::Client probe("127.0.0.2", port);
            probe.set_read_timeout(1);
            auto r = probe.Get("/health");
            CHECK(!(r && r->status == 200));   // nothing of Judas answers there yet
        }
        close(squat);
        bool bound = false;
        for (int i = 0; i < 100 && !bound; i++) {   // retry period is 3 s
            auto r = extra.Get("/health");
            bound = r && r->status == 200 && r->body.find("fake") != std::string::npos;
            if (!bound) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK(bound);
        s2.stop();
        t2.join();
        CHECK(ran2);
    }

    server.stop();
    th.join();
    CHECK(ran);
    fs::remove_all(base);
    TEST_MAIN_END();
}
