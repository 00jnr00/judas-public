// GET /api/daily-question: one question per local day, deterministic, no em dashes; plus the chat formatting
// blocks being described in the base prompt and the shared web helper being embedded.
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include "agent/agent.h"
#include "cpp-httplib/httplib.h"
#include "server/daily_question.h"
#include "server/server.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

class NullEngine : public Engine {
public:
    ChatResult chat(const ChatRequest &, const std::function<bool(const ChatDelta &)> &, const std::atomic<bool> *) override {
        ChatResult r;
        r.message.role = "assistant";
        r.message.content = "ok";
        r.finish_reason = "stop";
        return r;
    }
    EngineState state() const override {
        EngineState s;
        s.model = "fake";
        s.loaded = true;
        s.max_agents = 2;
        s.ctx_total = 8000;
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

int main() {
    // the list itself
    const auto & qs = daily_questions();
    CHECK(qs.size() >= 60);
    std::set<std::string> uniq(qs.begin(), qs.end());
    CHECK(uniq.size() == qs.size());
    for (auto & q : qs) {
        CHECK(q.find("\xE2\x80\x94") == std::string::npos);   // no em dash
        CHECK(!q.empty() && q.back() == '?');
    }
    // deterministic per date, and consecutive days differ
    CHECK(daily_question_for(2026, 11, 3) == daily_question_for(2026, 11, 3));
    CHECK(daily_question_for(2026, 11, 3) != daily_question_for(2026, 11, 4));
    CHECK(daily_day_number(1970, 1, 1) == 0);
    CHECK(daily_day_number(2026, 11, 4) - daily_day_number(2026, 11, 3) == 1);
    int y, m, d;
    CHECK(daily_parse_date("2026-02-28", y, m, d) && y == 2026 && m == 2 && d == 28);
    CHECK(!daily_parse_date("2026-02-30", y, m, d));
    CHECK(!daily_parse_date("2026-2-3", y, m, d));
    CHECK(!daily_parse_date("not a date", y, m, d));

    // the endpoint
    fs::path base = fs::path(JUDAS_TEST_TMP) / ("daily-q-" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base);
    Config cfg;
    cfg.data_dir = (base / "data").string();
    cfg.agents.role_dirs = {};
    cfg.skills.paths = {(base / "skills").string()};
    cfg.skills.track_usage = false;
    cfg.memory.path = (base / "SHARED.md").string();
    cfg.mcp.enabled = false;
    cfg.learning.review = false;
    cfg.daily_question.hour = 7;
    cfg.server.port = 19000 + getpid() % 500;
    NullEngine engine;
    Store store((base / "sessions.db").string());
    Journal journal(base / "snap");
    AgentManager agents(cfg, engine, store, journal);
    Server server(cfg, engine, agents, store, journal);
    std::thread th([&] { server.run(); });
    httplib::Client cli("127.0.0.1", cfg.server.port);
    cli.set_read_timeout(10);
    bool up = false;
    for (int i = 0; i < 100 && !up; i++) {
        auto r = cli.Get("/health");
        up = r && r->status == 200;
        if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(up);
    {
        auto a = cli.Get("/api/daily-question?date=2026-11-03");
        auto b = cli.Get("/api/daily-question?date=2026-11-03");
        auto c = cli.Get("/api/daily-question?date=2026-11-04");
        REQUIRE(a && b && c && a->status == 200);
        json ja = json::parse(a->body), jc = json::parse(c->body);
        CHECK(a->body == b->body);
        CHECK(ja["date"] == "2026-11-03");
        CHECK(ja["question"] == daily_question_for(2026, 11, 3));
        CHECK(ja["question"] != jc["question"]);
        CHECK(ja["enabled"] == true);
        CHECK(ja["hour"] == 7);
        auto bad = cli.Get("/api/daily-question?date=nope");
        CHECK(bad && bad->status == 400);
        auto today = cli.Get("/api/daily-question");   // no date: local today
        REQUIRE(today && today->status == 200);
        CHECK(json::parse(today->body)["date"].get<std::string>().size() == 10);
        // the shared web helper is served (embedded) for both pages
        auto js = cli.Get("/rich.js");
        CHECK(js && js->status == 200 && js->body.find("JudasRich") != std::string::npos);
        auto mp = cli.Get("/m/");
        CHECK(mp && mp->status == 200 && mp->body.find("/rich.js") != std::string::npos);
    }
    server.stop();
    th.join();

    // the base prompt tells the model about the chat blocks
    std::string src = slurp(fs::path(JUDAS_TESTS_DIR) / ".." / "src" / "agent" / "agent.cpp");
    CHECK(src.find("```cards") != std::string::npos);
    CHECK(src.find("```swatch") != std::string::npos);
    CHECK(src.find("- [ ] item") != std::string::npos);
    TEST_MAIN_END();
}
