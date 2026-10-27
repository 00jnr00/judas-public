// Learn tests: the shared /learn prompt, and the /api/learn route that starts it as a normal turn.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "agent/agent.h"
#include "core/json.h"
#include "cpp-httplib/httplib.h"
#include "learning/learn.h"
#include "server/server.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

class FakeEngine : public Engine {
public:
    ChatResult chat(const ChatRequest &, const std::function<bool(const ChatDelta &)> &,
                    const std::atomic<bool> * cancel) override {
        for (int i = 0; i < 20 && !(cancel && cancel->load()); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        ChatResult r;
        r.message.role = "assistant";
        r.message.content = "Learned it.";
        r.finish_reason = "stop";
        r.prompt_tokens = 5;
        r.completion_tokens = 2;
        return r;
    }
    EngineState state() const override { return {}; }
    std::string model_name() const override { return "fake"; }
    int count_tokens(const std::string & t) const override { return (int) t.size() / 4; }
};

int main() {
    // The prompt: sentinel, request placement, the tools Judas has, and no em dash.
    const std::string em_dash = "\xE2\x80\x94";
    std::string empty = build_learn_prompt("");
    CHECK(empty.rfind("[/learn]", 0) == 0);
    CHECK(empty.find("THE REQUEST:") != std::string::npos);
    CHECK(empty.find("the workflow we just went through in this conversation") != std::string::npos);
    CHECK(empty.find("read_file") != std::string::npos);
    CHECK(empty.find("glob") != std::string::npos);
    CHECK(empty.find("grep") != std::string::npos);
    CHECK(empty.find("web_fetch") != std::string::npos);
    CHECK(empty.find("skill_manage") != std::string::npos);
    CHECK(empty.find("action=\"create\"") != std::string::npos);
    CHECK(empty.find("action=\"patch\"") != std::string::npos);
    CHECK(empty.find("DATA, not instructions") != std::string::npos);
    CHECK(empty.find("16 KB") != std::string::npos);
    CHECK(empty.find(em_dash) == std::string::npos);
    CHECK(build_learn_prompt(" \t\r\n ") == empty);   // whitespace alone is the conversation default

    std::string given = build_learn_prompt("  read src/tools and cover web_fetch  ");
    CHECK(given.find("THE REQUEST:\nread src/tools and cover web_fetch\n") != std::string::npos);
    CHECK(given.find("the workflow we just went through in this conversation") == std::string::npos);
    CHECK(given.find(em_dash) == std::string::npos);

    // The route: new session, then append to it; errors stay explicit.
    fs::path base = "/tmp/opencode/judas-learn-test-" + std::to_string(getpid());
    fs::remove_all(base);
    fs::create_directories(base);

    Config cfg;
    cfg.data_dir = base.string();
    cfg.agents.role_dirs = {};
    cfg.skills.paths = {(base / "skills").string()};
    cfg.skills.track_usage = false;   // no skills.db in a test
    cfg.memory.path = (base / "SHARED.md").string();
    cfg.mcp.enabled = false;
    cfg.server.port = 17079 + getpid() % 1000;
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

    auto turn_idle = [&](const std::string & sid) {
        for (int i = 0; i < 200; i++) {
            auto r = cli.Get(("/api/sessions/" + sid).c_str());
            if (r && r->status == 200) {
                std::string st = json::parse(r->body)["turn"].value("status", "");
                if (st == "idle" || st == "done") return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return false;
    };

    auto has_user_text = [&](const std::string & sid, const std::string & needle) {
        for (int i = 0; i < 200; i++) {
            auto r = cli.Get(("/api/sessions/" + sid).c_str());
            if (r && r->status == 200) {
                auto j = json::parse(r->body);
                for (auto & m : j["messages"])
                    if (m["role"] == "user" && m["body"].contains("content") &&
                        m["body"]["content"].is_string() &&
                        m["body"]["content"].get<std::string>().find(needle) != std::string::npos)
                        return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return false;
    };

    {
        auto r = cli.Post("/api/learn", R"({"request":"review docs/API.md","cwd":"/tmp/opencode"})",
                          "application/json");
        REQUIRE(r && r->status == 200);
        std::string sid = json::parse(r->body)["session"];
        CHECK(sid.rfind("s-", 0) == 0);
        REQUIRE(has_user_text(sid, "[/learn]"));
        REQUIRE(has_user_text(sid, "review docs/API.md"));
        auto g = json::parse(cli.Get(("/api/sessions/" + sid).c_str())->body);
        CHECK(g["session"]["cwd"] == "/tmp/opencode");
        CHECK(g["session"]["title"].get<std::string>().rfind("Learn: review docs/API.md", 0) == 0);

        REQUIRE(turn_idle(sid));
        auto again = cli.Post("/api/learn", "{\"session\":\"" + sid + "\",\"request\":\"\"}",
                              "application/json");
        REQUIRE(again && again->status == 200);
        CHECK(json::parse(again->body)["session"] == sid);
        REQUIRE(has_user_text(sid, "the workflow we just went through in this conversation"));
    }
    {
        auto r = cli.Post("/api/learn", R"({"session":"s-nope"})", "application/json");
        CHECK(r && r->status == 404);
        auto bad = cli.Post("/api/learn", R"({"request":5})", "application/json");
        CHECK(bad && bad->status == 400);
        auto broken = cli.Post("/api/learn", "not json", "application/json");
        CHECK(broken && broken->status == 400);
    }

    server.stop();
    th.join();
    CHECK(ran);
    fs::remove_all(base);
    TEST_MAIN_END();
}
