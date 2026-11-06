// Server tests: FakeEngine behind the real Store, Journal and AgentManager, hit over HTTP.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "agent/autoresume.h"
#include "artifacts/artifacts.h"
#include "artifacts/raster.h"
#include "cpp-httplib/httplib.h"
#include "server/server.h"
#include "tutor/learn_store.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

class FakeEngine : public Engine {
public:
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on_delta,
                    const std::atomic<bool> * cancel) override {
        for (auto & m : req.messages)   // tool-advertising probe: a user message "toolprobe-<mode>" records the tool names offered
            if (m.role == "user" && m.content.rfind("toolprobe-", 0) == 0) {
                std::lock_guard lk(probe_mu);
                for (auto & t : req.tools) probed[m.content].insert(t.name);
            }
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

    std::mutex probe_mu;
    std::map<std::string, std::set<std::string>> probed;

private:
    std::atomic<int> calls_{0};
};

int main() {
    setenv("JUDAS_AUTO_RESUME_TICK_S", "1", 1);
    fs::path base = JUDAS_TEST_TMP "/judas-server-test-" + std::to_string(getpid());
    fs::remove_all(base);
    fs::create_directories(base);

    // stand-in for the Learn visual tutor (an app that uses absolute paths and cannot move under a sub-path)
    httplib::Server tutor;
    tutor.Get("/", [](const httplib::Request & r, httplib::Response & res) {
        res.set_content("<link href=\"/_next/a.css\">tutor home host=" + r.get_header_value("Host"), "text/html");
        res.set_header("X-Frame-Options", "DENY");
    });
    tutor.Get("/stream", [](const httplib::Request &, httplib::Response & res) {
        auto n = std::make_shared<int>(0);
        res.set_chunked_content_provider("text/event-stream", [n](size_t, httplib::DataSink & sink) {
            int i = (*n)++;
            if (i >= 3) { sink.done(); return true; }
            std::string c = "data: " + std::to_string(i) + "\n\n";
            sink.write(c.data(), c.size());
            return true;
        });
    });
    tutor.Post("/api/echo", [](const httplib::Request & r, httplib::Response & res) {
        res.set_content(r.get_header_value("Content-Type") + "|" + r.get_header_value("Origin") + "|" + r.body, "text/plain");
    });
    int tutor_port = tutor.bind_to_any_port("127.0.0.1");
    tutor.Get("/go", [&tutor_port](const httplib::Request &, httplib::Response & res) {
        res.set_redirect("http://127.0.0.1:" + std::to_string(tutor_port) + "/x", 302);
    });
    std::thread tutor_th([&] { tutor.listen_after_bind(); });

    Config cfg;
    cfg.data_dir = (base / "data").string();   // Learn topics are written here, never to the real data dir
    cfg.learn.show_visual = true;
    cfg.learn.visual_url = "http://127.0.0.1:" + std::to_string(tutor_port);
    cfg.learn.visual_proxy_port = 22000 + getpid() % 1000;
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
    {   // bundled web fonts: embedded, woff2 content type, magic bytes, CSS lists them, CSP allows font-src 'self'
        auto css = cli.Get("/fonts.css");
        REQUIRE(css && css->status == 200);
        CHECK(css->get_header_value("Content-Type").find("text/css") == 0);
        CHECK(css->body.find("\"Inter\"") != std::string::npos && css->body.find("font-display:swap") != std::string::npos);
        size_t pos = css->body.find("url(\"/fonts/");
        REQUIRE(pos != std::string::npos);
        int n_fonts = 0;
        while (pos != std::string::npos) {
            size_t a = pos + 5, b = css->body.find('"', a);
            std::string url = css->body.substr(a, b - a);
            auto f = cli.Get(url);
            REQUIRE(f && f->status == 200);
            CHECK(f->get_header_value("Content-Type") == "font/woff2");
            CHECK(f->body.size() > 1000 && f->body.compare(0, 4, "wOF2") == 0);
            n_fonts++;
            pos = css->body.find("url(\"/fonts/", b);
        }
        CHECK(n_fonts >= 30);
        auto idx = cli.Get("/");
        REQUIRE(idx && idx->status == 200);
        CHECK(idx->get_header_value("Content-Security-Policy").find("font-src 'self'") != std::string::npos);
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
    {   // phone UI: /m redirects, /m/ serves its page, with the frame CSP
        auto rd = cli.Get("/m");
        REQUIRE(rd && rd->status == 302);
        CHECK(rd->get_header_value("Location").find("/m/") != std::string::npos);
        auto r = cli.Get("/m/");
        REQUIRE(r && r->status == 200);
        CHECK(r->body.find("m.js") != std::string::npos);
        CHECK(r->get_header_value("Content-Security-Policy").find("frame-src") != std::string::npos);
        auto js = cli.Get("/m/m.js");
        REQUIRE(js && js->status == 200);
    }
    {   // Learn topics: list and single topic come from <data_dir>/learn/topics.json
        auto e0 = cli.Get("/api/learn/topics");
        REQUIRE(e0 && e0->status == 200);
        CHECK(json::parse(e0->body)["topics"].empty());
        json doc = json::object();
        bool ok = false;
        learn_plan_apply(doc, {{"topic", "Hash tables"}, {"phase", "teach"},
                               {"steps", {{{"id", "a"}, {"title", "Hash"}, {"status", "done"}}, {{"id", "b"}, {"title", "Collide"}, {"depends_on", json::array({"a"})}}}}},
                         &ok, "2026-11-03", "sess1");
        CHECK(ok);
        learn_note_apply(doc, {{"topic", "Hash tables"}, {"notes", {{{"text", "A hash maps keys to slots"}}}}}, &ok, "2026-11-03", "sess1");
        CHECK(ok);
        std::string err;
        CHECK(learn_topics_save(cfg, doc, &err));
        auto e1 = cli.Get("/api/learn/topics");
        REQUIRE(e1 && e1->status == 200);
        auto tl = json::parse(e1->body)["topics"];
        REQUIRE(tl.size() == 1);
        CHECK(tl[0]["id"] == "hash-tables" && tl[0]["steps_total"] == 2 && tl[0]["steps_done"] == 1 && tl[0]["props"] == 1 && tl[0]["session"] == "sess1");
        auto e2 = cli.Get("/api/learn/topic/hash-tables");
        REQUIRE(e2 && e2->status == 200);
        auto tj = json::parse(e2->body);
        CHECK(tj["phase"] == "teach" && tj["steps"].size() == 2 && tj["props"][0]["id"] == "p1" && tj["id"] == "hash-tables");
        auto e3 = cli.Get("/api/learn/topic/nope");
        REQUIRE(e3 && e3->status == 404);
    }
    {   // Learn visual tutor: advertised proxy port, forwarded verbatim (root paths, streaming, bodies), host-checked
        auto v = cli.Get("/api/learn/visual");
        REQUIRE(v && v->status == 200);
        auto vj = json::parse(v->body);
        CHECK(vj["up"] == true && vj["proxy_port"] == cfg.learn.visual_proxy_port);
        auto idx = cli.Get("/m/");
        CHECK(idx->get_header_value("Content-Security-Policy").find("http://*:" + std::to_string(cfg.learn.visual_proxy_port)) != std::string::npos);
        httplib::Client px("127.0.0.1", cfg.learn.visual_proxy_port);
        px.set_read_timeout(10);
        bool pup = false;
        for (int i = 0; i < 100 && !pup; i++) {
            auto r = px.Get("/");
            pup = r && r->status == 200;
            if (!pup) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        CHECK(pup);
        auto r = px.Get("/");
        REQUIRE(r && r->status == 200);
        CHECK(r->body.find("/_next/a.css") != std::string::npos);
        CHECK(r->body.find("host=127.0.0.1:" + std::to_string(tutor_port)) != std::string::npos);   // upstream sees its own host
        CHECK(r->get_header_value("X-Frame-Options").empty());   // Judas pages frame it
        auto st = px.Get("/stream");
        REQUIRE(st && st->status == 200);
        CHECK(st->body == "data: 0\n\ndata: 1\n\ndata: 2\n\n");
        CHECK(st->get_header_value("Content-Type").find("text/event-stream") == 0);
        auto po = px.Post("/api/echo", httplib::Headers{{"Origin", "http://127.0.0.1:" + std::to_string(cfg.learn.visual_proxy_port)}}, "{\"a\":1}", "application/json");
        REQUIRE(po && po->status == 200);
        CHECK(po->body == "application/json|http://127.0.0.1:" + std::to_string(tutor_port) + "|{\"a\":1}");
        auto nf = px.Get("/missing");
        REQUIRE(nf);
        CHECK(nf->status == 404);
        auto rd = px.Get("/go");
        REQUIRE(rd);
        CHECK(rd->status == 302 && rd->get_header_value("Location") == "/x");
        auto bad = px.Get("/", httplib::Headers{{"Host", "evil.example"}});
        REQUIRE(bad);
        CHECK(bad->status == 403);
        // a page on another site cannot write through the forwarder (foreign Origin, or cross-site with no Origin)
        auto xo = px.Post("/api/echo", httplib::Headers{{"Origin", "https://evil.example"}}, "{}", "application/json");
        REQUIRE(xo);
        CHECK(xo->status == 403);
        auto xs = px.Post("/api/echo", httplib::Headers{{"Sec-Fetch-Site", "cross-site"}}, "{}", "text/plain");
        REQUIRE(xs);
        CHECK(xs->status == 403);
        auto xn = px.Post("/api/echo", httplib::Headers{{"Origin", "null"}}, "{}", "text/plain");
        REQUIRE(xn);
        CHECK(xn->status == 403);
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
        // a browser-marked cross-site write with no Origin to vet is refused too
        auto xsite = cli.Post("/api/learn", httplib::Headers{{"Sec-Fetch-Site", "cross-site"}}, "{}", "text/plain");
        CHECK(xsite && xsite->status == 403);
        // the app cannot be framed or script-injected from elsewhere
        auto page = cli.Get("/");
        REQUIRE(page);
        std::string csp = page->get_header_value("Content-Security-Policy");
        CHECK(csp.find("frame-ancestors 'self'") != std::string::npos && csp.find("script-src 'self'") != std::string::npos &&
              csp.find("object-src 'none'") != std::string::npos && csp.find("base-uri 'self'") != std::string::npos);
        CHECK(csp.find("frame-ancestors 'self'; frame-src 'self'") != std::string::npos);   // configured origins join frame-src only
        CHECK(page->get_header_value("X-Frame-Options") == "SAMEORIGIN" && page->get_header_value("X-Content-Type-Options") == "nosniff");
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
        auto cl = json::parse(cli.Post("/api/sessions", R"({"mode":"learn"})", "application/json")->body);
        CHECK(cl["mode"] == "learn");
        auto cbad = cli.Post("/api/sessions", R"({"mode":"zzz"})", "application/json");
        CHECK(cbad && cbad->status == 400);

        auto ce = cli.Post("/api/sessions", R"({"mode":"engineering","cwd":"/tmp"})", "application/json");
        REQUIRE(ce && ce->status == 200);
        CHECK(json::parse(ce->body)["mode"] == "engineering");

        {   // Cowork work modes: ids, per-session state endpoints, the Write document view, artifacts by session
            fs::path wdir = base / "cowork-docs";
            fs::create_directories(wdir);
            { std::ofstream(wdir / "d.txt") << "A teh cat.\n"; }
            std::string wsid;
            for (const char * m : {"design", "research", "write"}) {
                auto cr2 = cli.Post("/api/sessions", std::string("{\"mode\":\"") + m + "\",\"cwd\":\"" + wdir.string() + "\"}", "application/json");
                REQUIRE(cr2 && cr2->status == 200);
                auto sj = json::parse(cr2->body);
                CHECK(sj["mode"] == m);
                wsid = sj["id"];
            }
            CHECK(cli.Post("/api/sessions", R"({"mode":"drawing"})", "application/json")->status == 400);
            std::string pre = "/api/sessions/" + wsid;
            CHECK(json::parse(cli.Get(pre + "/calcs")->body)["calcs"].empty());
            CHECK(json::parse(cli.Get(pre + "/sources")->body)["sources"].empty());
            CHECK(json::parse(cli.Get(pre + "/suggestions")->body)["suggestions"].empty());
            CHECK(cli.Get("/api/sessions/nope/calcs")->status == 404);
            CalcRow cr; cr.name = "n"; cr.expr = "e"; cr.value = "1";
            store.add_calc(wsid, cr);
            auto cj2 = json::parse(cli.Get(pre + "/calcs")->body);
            REQUIRE(cj2["calcs"].size() == 1);
            CHECK(cj2["calcs"][0]["n"] == 1 && cj2["calcs"][0]["name"] == "n" && cj2["calcs"][0]["created_ms"].get<long long>() > 0);
            SourceRow sr; sr.url = "https://x.example/"; sr.title = "X"; sr.autoadded = true;
            store.add_source(wsid, sr);
            auto sj2 = json::parse(cli.Get(pre + "/sources")->body);
            CHECK(sj2["sources"][0]["auto"] == true && sj2["sources"][0]["url"] == "https://x.example/");

            // file GET / PUT
            auto fg = cli.Get(pre + "/file?path=d.txt");
            REQUIRE(fg && fg->status == 200);
            CHECK(fg->body == "A teh cat.\n" && fg->get_header_value("X-Judas-Hash").size() == 64);
            auto fm = json::parse(cli.Get(pre + "/file?path=d.txt&meta=1")->body);
            CHECK(fm["text"] == "A teh cat.\n" && fm["hash"] == fg->get_header_value("X-Judas-Hash"));
            CHECK(cli.Get(pre + "/file?path=..%2Fx")->status == 403);
            CHECK(cli.Get(pre + "/file?path=nope.txt")->status == 404);
            std::string h0 = fm["hash"];
            auto pu = cli.Put(pre + "/file?path=d.txt", json({{"text", "A the cat.\n"}, {"base_hash", h0}}).dump(), "application/json");
            REQUIRE(pu && pu->status == 200);
            std::string h1 = json::parse(pu->body)["hash"];
            auto pc = cli.Put(pre + "/file?path=d.txt", json({{"text", "clobber"}, {"base_hash", h0}}).dump(), "application/json");
            CHECK(pc && pc->status == 409 && json::parse(pc->body)["hash"] == h1);
            CHECK(cli.Get(pre + "/file?path=d.txt")->body == "A the cat.\n");

            // suggestions: accept exactly once, then stale; reject
            { std::ofstream(wdir / "d.txt") << "A teh cat.\n"; }
            SuggestionRow g; g.path = "d.txt"; g.kind = "spelling"; g.find = "teh"; g.replace = "the"; g.why = "typo";
            std::string gid = store.add_suggestion(wsid, g);
            auto lg = json::parse(cli.Get(pre + "/suggestions")->body);
            CHECK(lg["suggestions"][0]["id"] == gid && lg["suggestions"][0]["status"] == "open");
            auto ac = cli.Post(pre + "/suggestions/" + gid + "/accept", "", "application/json");
            REQUIRE(ac && ac->status == 200);
            CHECK(json::parse(ac->body)["suggestion"]["status"] == "accepted");
            CHECK(cli.Get(pre + "/file?path=d.txt")->body == "A the cat.\n");
            CHECK(cli.Post(pre + "/suggestions/" + gid + "/accept", "", "application/json")->status == 409);
            std::string gid2 = store.add_suggestion(wsid, g);   // find "teh" is gone now
            auto st2 = cli.Post(pre + "/suggestions/" + gid2 + "/accept", "", "application/json");
            CHECK(st2 && st2->status == 409 && json::parse(st2->body)["suggestion"]["status"] == "stale");
            std::string gid3 = store.add_suggestion(wsid, g);
            CHECK(cli.Post(pre + "/suggestions/" + gid3 + "/reject", "", "application/json")->status == 200);
            CHECK(cli.Post(pre + "/suggestions/g99/reject", "", "application/json")->status == 404);

            // cad: the live model of the process (none yet); the STL is 404 until a part exists
            auto cd2 = cli.Get(pre + "/cad");
            REQUIRE(cd2 && cd2->status == 200);
            CHECK(json::parse(cd2->body).contains("available"));
            CHECK(cli.Get(pre + "/cad/model.stl")->status == 404);

            // artifacts by originating session
            ArtifactStore as(cfg);
            std::string aerr;
            auto am = as.create("T", "markdown", {{"r.md", "# r"}}, wsid, "", aerr);
            REQUIRE(am.has_value());
            auto al = json::parse(cli.Get("/api/artifacts?session=" + wsid)->body);
            REQUIRE(al["artifacts"].size() == 1);
            CHECK(al["artifacts"][0]["id"] == am->id);
            CHECK(json::parse(cli.Get("/api/artifacts?session=s-none")->body)["artifacts"].empty());
            CHECK(as.remove(am->id));   // later checks count the artifacts exactly
        }

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
                if (m["role"] == "user" && (m["body"].dump().find("Resume the task") != std::string::npos ||
                                            m["body"].dump().find("The last resumes made no progress") != std::string::npos)) n++;   // a wrap-up resume counts too
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

    // --- a session started through the API reaches the feed and the list in its own mode ---
    {
        std::string seen;
        std::thread feed([&] {
            httplib::Client fc("127.0.0.1", cfg.server.port);
            fc.set_read_timeout(8, 0);
            fc.Get("/api/events", [&](const char * d, size_t n) {
                seen.append(d, n);
                return seen.find("api-visible-probe") == std::string::npos;
            });
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        auto r = cli.Post("/api/sessions/new/messages", R"({"content":"api-visible-probe","mode":"learn"})",
                          "application/json");
        REQUIRE(r && r->status == 200);
        std::string sid = json::parse(r->body)["session"];
        feed.join();
        CHECK(seen.find("session.created") != std::string::npos);
        CHECK(seen.find("\"mode\":\"learn\"") != std::string::npos);
        CHECK(seen.find(sid) != std::string::npos);
        auto list = json::parse(cli.Get("/api/sessions?limit=1000")->body);
        bool found = false;
        for (auto & x : list["sessions"]) if (x["id"] == sid) { found = true; CHECK(x["mode"] == "learn"); }
        CHECK(found);
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

    // GET /api/sessions/search: one hit per session with a bracketed snippet; punctuation cannot break MATCH
    {
        auto a = store.create_session("Budget chat", "", "fake");
        auto b = store.create_session("Other", "", "fake", "code");
        store.add_message(a.id, "", "assistant", json{{"content", "The 50/30/20 rule splits needs and wants"}});
        store.add_message(a.id, "", "user", json{{"content", "wants again, wants more"}});
        store.add_message(b.id, "", "user", json{{"content", "nothing relevant here"}});
        auto r = cli.Get("/api/sessions/search?q=wants");
        REQUIRE(r && r->status == 200);
        json j = json::parse(r->body);
        CHECK(j["hits"].size() == 1);
        CHECK(j["hits"][0]["session"] == a.id);
        auto sysr = cli.Get("/api/sessions/search?q=final%20answer");   // injected system prompts are never hits
        CHECK(sysr && json::parse(sysr->body)["hits"].empty());
        CHECK(j["hits"][0]["title"] == "Budget chat");
        CHECK(j["hits"][0]["snippet"].get<std::string>().find("[wants]") != std::string::npos);
        for (const char * q : {"%22", "NEAR(", "a%20OR%20", "%2A", "%28%29%22%22", "", "x%27--"}) {
            auto rr = cli.Get(std::string("/api/sessions/search?q=") + q);
            CHECK(rr && rr->status == 200 && json::parse(rr->body)["hits"].is_array());
        }
        auto g = cli.Get("/api/sessions/" + a.id);   // the id route still works next to /search
        CHECK(g && g->status == 200);
    }

    // GET /prompts.json: the user's private file wins when it is a JSON object, else the embedded pool; no-cache
    {
        fs::create_directories(base / "home" / ".config" / "judas");
        setenv("HOME", (base / "home").c_str(), 1);
        setenv("JUDAS_CONFIG", (base / "home" / ".config" / "judas" / "config.toml").c_str(), 1);
        fs::path pj = base / "home" / ".config" / "judas" / "prompts.json";
        auto r0 = cli.Get("/prompts.json");
        CHECK(r0 && r0->status == 200 && r0->get_header_value("Cache-Control") == "no-cache");
        std::string embedded = r0 ? r0->body : "";
        std::ofstream(pj) << R"({"chat":[{"title":"mine","prompt":"private prompt"}]})";
        auto r1 = cli.Get("/prompts.json");
        CHECK(r1 && r1->status == 200 && r1->body.find("private prompt") != std::string::npos);
        CHECK(r1 && r1->get_header_value("Cache-Control") == "no-cache");
        std::ofstream(pj) << "not json";
        auto r2 = cli.Get("/prompts.json");
        CHECK(r2 && r2->status == 200 && r2->body == embedded);
        std::ofstream(pj) << "[1,2]";
        auto r3 = cli.Get("/prompts.json");
        CHECK(r3 && r3->body == embedded);
    }

    // Tainted sessions: health_tainted is visible to every caller; with X-Judas-Client: mcp the server withholds all
    // content (messages, agent task/result, title, queued text, search hits, event streams). The web UI is unchanged.
    {
        auto t = store.create_session("my resting heart rate", "", "fake");
        auto c2 = store.create_session("ordinary chat", "", "fake");
        auto ag = store.create_agent(t.id, "", "lead", "check my HRV numbers");
        ag.result = "HRV averaged 41 ms";
        store.update_agent(ag);
        store.add_message(t.id, ag.id, "assistant", json{{"content", "resting HR 64 healthhit"}});
        store.add_message(c2.id, "", "assistant", json{{"content", "plain healthhit"}});
        store.set_session_health_tainted(t.id);
        httplib::Headers mcp = {{"X-Judas-Client", "mcp"}};

        auto web = cli.Get("/api/sessions/" + t.id);
        REQUIRE(web && web->status == 200);
        json w = json::parse(web->body);
        CHECK(w["session"]["health_tainted"] == true && w["session"]["title"] == "my resting heart rate");
        CHECK(w["messages"].size() == 1 && w["agents"][0]["result"] == "HRV averaged 41 ms" && !w.contains("content_withheld"));
        auto clean = cli.Get("/api/sessions/" + c2.id);
        CHECK(clean && json::parse(clean->body)["session"]["health_tainted"] == false);

        auto m = cli.Get("/api/sessions/" + t.id, mcp);
        REQUIRE(m && m->status == 200);
        json mj = json::parse(m->body);
        CHECK(mj["messages"].empty() && mj["agents"][0]["task"] == "" && mj["agents"][0]["result"] == "");
        CHECK(mj["session"]["title"].get<std::string>().find("open it in the Judas app") != std::string::npos);
        CHECK(mj["content_withheld"].is_string() && mj["session"]["health_tainted"] == true && mj["total"] == 1);
        CHECK(m->body.find("resting HR") == std::string::npos && m->body.find("HRV") == std::string::npos);
        auto mc = cli.Get("/api/sessions/" + c2.id, mcp);
        CHECK(mc && json::parse(mc->body)["messages"].size() == 1);

        auto lw = cli.Get("/api/sessions?limit=50");
        CHECK(lw && lw->body.find("my resting heart rate") != std::string::npos);
        auto lm = cli.Get("/api/sessions?limit=50", mcp);
        CHECK(lm && lm->body.find("my resting heart rate") == std::string::npos && lm->body.find("ordinary chat") != std::string::npos);
        auto sw = cli.Get("/api/sessions/search?q=healthhit");
        CHECK(sw && json::parse(sw->body)["hits"].size() == 2);
        auto sm = cli.Get("/api/sessions/search?q=healthhit", mcp);
        CHECK(sm && json::parse(sm->body)["hits"].size() == 1);
        auto ev = cli.Get("/api/sessions/" + t.id + "/events", mcp);
        CHECK(ev && ev->status == 403);
        auto gev = cli.Get("/api/events", mcp);
        CHECK(gev && gev->status == 403);
    }

    // Health Link: /api/health/status works with the link off; /api/health/weekly validates, persists to config.toml
    // (the rest of the file kept) and flips the live switch.
    {
        auto st = cli.Get("/api/health/status");
        REQUIRE(st && st->status == 200);
        json sj = json::parse(st->body);
        CHECK(sj["configured"] == false && sj["reachable"] == false && sj.contains("weekly_checkin"));
        auto bad = cli.Post("/api/health/weekly", "{\"enabled\":\"yes\"}", "application/json");
        CHECK(bad && bad->status == 400);
        fs::path cfgf = base / "home" / ".config" / "judas" / "config.toml";
        std::ofstream(cfgf) << "# keep me\n[health]\nurl = \"http://phone:8765\"\n";
        auto off = cli.Post("/api/health/weekly", "{\"enabled\":false}", "application/json");
        CHECK(off && off->status == 200 && json::parse(off->body)["weekly_checkin"] == false);
        std::ifstream in(cfgf);
        std::string txt((std::istreambuf_iterator<char>(in)), {});
        CHECK(txt.find("# keep me") != std::string::npos && txt.find("weekly_checkin = false") != std::string::npos &&
              txt.find("http://phone:8765") != std::string::npos);
        auto st2 = cli.Get("/api/health/status");
        CHECK(st2 && json::parse(st2->body)["weekly_checkin"] == false);
        auto on = cli.Post("/api/health/weekly", "{\"enabled\":true}", "application/json");
        CHECK(on && json::parse(on->body)["weekly_checkin"] == true);
    }


    // Artifacts: served from /a/<id>/ under a CSP sandbox; the API stays out of reach of sandboxed (null origin) pages.
    {
        ArtifactStore as(cfg);
        std::string aerr;
        auto am = as.create("Sandbox demo", "html", {{"index.html", "<html><body><h1>hi</h1></body></html>"}, {"app.js", "var x=1;"}}, "s1", "", aerr);
        REQUIRE(am.has_value());
        as.update(am->id, {{"index.html", "<html><body><h1>v2</h1></body></html>"}}, {}, "second", "s1", aerr);
        std::string stl = "solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid t\n";
        auto mm = as.create("Tri", "model3d", {{"model.stl", stl}}, "s1", "", aerr);
        REQUIRE(mm.has_value());
        std::string base_a = "/a/" + am->id;

        auto r = cli.Get(base_a + "/");
        REQUIRE(r && r->status == 200);
        CHECK(r->body.find("v2") != std::string::npos && r->body.find("/vendor/artifacts/capture.js") != std::string::npos);
        std::string csp = r->get_header_value("Content-Security-Policy");
        CHECK(csp.find("sandbox allow-scripts") != std::string::npos && csp.find("allow-same-origin") == std::string::npos);
        CHECK(csp.find("default-src 'none'") != std::string::npos && csp.find("frame-ancestors 'self'") != std::string::npos);
        CHECK(csp.find("http://") == std::string::npos && csp.find("https://") == std::string::npos);   // no CDN, no foreign origin
        CHECK(r->get_header_value("X-Content-Type-Options") == "nosniff");
        CHECK(r->get_header_value("Content-Type").find("text/html") == 0);
        auto v1 = cli.Get(base_a + "/v1/");
        CHECK(v1 && v1->status == 200 && v1->body.find("<h1>hi</h1>") != std::string::npos);
        auto js = cli.Get(base_a + "/v1/app.js");
        CHECK(js && js->status == 200 && js->body == "var x=1;" && js->get_header_value("Content-Type").find("javascript") != std::string::npos);
        CHECK(js && js->get_header_value("Content-Security-Policy").find("sandbox") != std::string::npos);
        auto nos = cli.Get(base_a);
        CHECK(nos && nos->status == 302);
        auto nov = cli.Get(base_a + "/v2");
        CHECK(nov && nov->status == 302);
        CHECK(cli.Get(base_a + "/v9/") && cli.Get(base_a + "/v9/")->status == 404);
        CHECK(cli.Get(base_a + "/..%2Fmeta.json") && cli.Get(base_a + "/..%2Fmeta.json")->status == 404);
        CHECK(cli.Get(base_a + "/v1/..%2F..%2Fmeta.json") && cli.Get(base_a + "/v1/..%2F..%2Fmeta.json")->status == 404);
        CHECK(cli.Get(base_a + "/%2e%2e/%2e%2e/etc/passwd") && cli.Get(base_a + "/%2e%2e/%2e%2e/etc/passwd")->status == 404);
        CHECK(cli.Get("/a/zzzzzzzzzz/") && cli.Get("/a/zzzzzzzzzz/")->status == 404);
        // a sandboxed page (Origin: null) can read its own files but gets a CORS answer scoped to null
        auto nulo = cli.Get(base_a + "/v1/app.js", httplib::Headers{{"Origin", "null"}});
        CHECK(nulo && nulo->status == 200 && nulo->get_header_value("Access-Control-Allow-Origin") == "null");
        auto vend = cli.Get("/vendor/artifacts/three/three.module.min.js", httplib::Headers{{"Origin", "null"}});
        CHECK(vend && vend->status == 200 && vend->get_header_value("Access-Control-Allow-Origin") == "*");
        CHECK(vend && vend->body.size() > 100000);
        auto vcore = cli.Get("/vendor/artifacts/viewer3d.js");
        CHECK(vcore && vcore->status == 200 && vcore->body.find("three/addons") != std::string::npos);
        // ...and cannot reach the API, by fetch (Origin: null) or by tags (cross-site, no Origin)
        auto n1 = cli.Get("/api/artifacts", httplib::Headers{{"Origin", "null"}});
        CHECK(n1 && n1->status == 403);
        auto n2 = cli.Get("/api/sessions", httplib::Headers{{"Origin", "null"}});
        CHECK(n2 && n2->status == 403);
        auto n3 = cli.Post("/api/tasks", {{"Origin", "null"}}, "{\"prompt\":\"x\"}", "application/json");
        CHECK(n3 && n3->status == 403);
        auto n4 = cli.Get("/api/attachments/abcd", httplib::Headers{{"Sec-Fetch-Site", "cross-site"}, {"Sec-Fetch-Dest", "image"}});
        CHECK(n4 && n4->status == 403);
        auto n5 = cli.Get("/state", httplib::Headers{{"Sec-Fetch-Site", "cross-site"}, {"Sec-Fetch-Dest", "empty"}});
        CHECK(n5 && n5->status == 403);
        auto okc = cli.Get("/api/artifacts", httplib::Headers{{"Sec-Fetch-Site", "same-origin"}, {"Sec-Fetch-Dest", "empty"}});
        CHECK(okc && okc->status == 200);

        {   // edit a saved task
            auto mk = cli.Post("/api/tasks", "{\"prompt\":\"brief me\",\"title\":\"Morning\",\"kind\":\"manual\"}", "application/json");
            REQUIRE(mk && mk->status == 200);
            std::string tid = json::parse(mk->body)["id"];
            auto e1 = cli.Patch("/api/tasks/" + tid, "{\"prompt\":\"brief me better\",\"kind\":\"interval\",\"interval_s\":86400}", "application/json");
            REQUIRE(e1 && e1->status == 200);
            json j = json::parse(e1->body);
            CHECK(j["prompt"] == "brief me better" && j["title"] == "Morning" && j["kind"] == "interval" && j["next_ms"].get<long long>() > 0);
            auto e2 = cli.Patch("/api/tasks/" + tid, "{\"kind\":\"once\",\"run_at_ms\":1000}", "application/json");
            CHECK(e2 && e2->status == 400);   // in the past
            auto e3 = cli.Patch("/api/tasks/" + tid, "{\"prompt\":\"\"}", "application/json");
            CHECK(e3 && e3->status == 400);
            auto e4 = cli.Patch("/api/tasks/" + tid, "{\"kind\":\"manual\"}", "application/json");
            CHECK(e4 && e4->status == 200 && json::parse(e4->body)["next_ms"] == 0);
            auto e5 = cli.Patch("/api/tasks/nope", "{}", "application/json");
            CHECK(e5 && e5->status == 404);
            auto n6 = cli.Patch("/api/tasks/" + tid, {{"Origin", "null"}}, "{\"prompt\":\"x\"}", "application/json");
            CHECK(n6 && n6->status == 403);
            cli.Delete("/api/tasks/" + tid);
        }

        // JSON API
        auto lst = cli.Get("/api/artifacts");
        REQUIRE(lst && lst->status == 200);
        json lj = json::parse(lst->body);
        CHECK(lj["artifacts"].size() == 2);
        auto one = cli.Get("/api/artifacts/" + am->id);
        CHECK(one && one->status == 200 && json::parse(one->body)["latest"] == 2);
        CHECK(cli.Get("/api/artifacts/nope000000") && cli.Get("/api/artifacts/nope000000")->status == 404);
        auto q = cli.Get("/api/artifacts?q=tri");
        CHECK(q && json::parse(q->body)["artifacts"].size() == 1);
        // a 3D model gets a preview drawn inside judas
        auto th = cli.Get("/api/artifacts/" + mm->id + "/thumb");
        CHECK(th && th->status == 200 && th->get_header_value("Content-Type") == "image/png" && th->body.size() > 100);
        int tw = 0, thh = 0;
        CHECK(th && png_size(th->body, tw, thh) && tw == 480 && thh == 360);
        // html has none until a web page delivers a capture
        CHECK(cli.Get("/api/artifacts/" + am->id + "/thumb") && cli.Get("/api/artifacts/" + am->id + "/thumb")->status == 404);
        CHECK(cli.Post("/api/artifacts/" + am->id + "/render-result?version=2", "not a png", "image/png")->status == 400);
        Image tiny; tiny.w = 16; tiny.h = 16; tiny.rgba.assign(16 * 16 * 4, 200);
        auto up = cli.Post("/api/artifacts/" + am->id + "/render-result?version=2", encode_png(tiny), "image/png");
        CHECK(up && up->status == 200);
        auto th2 = cli.Get("/api/artifacts/" + am->id + "/thumb?v=2");
        CHECK(th2 && th2->status == 200 && png_size(th2->body, tw, thh) && tw == 16);
        CHECK(cli.Post("/api/artifacts/" + am->id + "/render-result?version=7", encode_png(tiny), "image/png")->status == 404);
        // download
        auto dl = cli.Get("/api/artifacts/" + am->id + "/download?v=1&path=app.js");
        CHECK(dl && dl->status == 200 && dl->body == "var x=1;" && dl->get_header_value("Content-Disposition").find("attachment") == 0);
        CHECK(cli.Get("/api/artifacts/" + am->id + "/download?path=..%2Fmeta.json")->status == 404);
        // delete
        auto del = cli.Delete("/api/artifacts/" + am->id);
        CHECK(del && del->status == 200);
        CHECK(cli.Get(base_a + "/")->status == 404);
        CHECK(cli.Delete("/api/artifacts/" + am->id)->status == 404);
        cli.Delete("/api/artifacts/" + mm->id);
    }


    // Artifact tools are offered to the lead in every session mode (Chat included), and artifacts made in one session are
    // visible from another (they live in the data dir, not in a session).
    {
        const char * modes[] = {"chat", "cowork", "code", "learn", "swarm"};
        for (const char * mode : modes) {
            std::string probe = std::string("toolprobe-") + mode;
            auto r = cli.Post("/api/sessions/new/messages", json{{"content", probe}, {"mode", mode}}.dump(), "application/json");
            REQUIRE(r && r->status == 200);
            std::string sid = json::parse(r->body)["session"];
            for (int i = 0; i < 200; i++) {
                auto g = cli.Get("/api/sessions/" + sid);
                if (g && g->status == 200 && json::parse(g->body)["turn"]["running"] == false) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::set<std::string> seen;
            { std::lock_guard lk(engine.probe_mu); seen = engine.probed[probe]; }
            for (const char * t : {"artifact_create", "artifact_update", "artifact_read", "artifact_list", "artifact_render"})
                if (!seen.count(t)) { std::fprintf(stderr, "mode %s does not offer %s\n", mode, t); judas_test_failures++; }
        }
        ArtifactStore as2(cfg);
        std::string e2;
        auto made = as2.create("From another session", "svg", {{"image.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'/>"}}, "other-session", "", e2);
        REQUIRE(made.has_value());
        auto lst = cli.Get("/api/artifacts");
        bool found = false;
        for (auto & a : json::parse(lst->body)["artifacts"]) if (a["id"] == made->id) found = true;
        CHECK(found);
        as2.remove(made->id);
    }

    server.stop();
    th.join();
    CHECK(ran);
    tutor.stop();
    tutor_th.join();
    fs::remove_all(base);
    TEST_MAIN_END();
}
