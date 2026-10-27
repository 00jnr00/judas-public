// MCP client against the fake server in tests/fake_mcp_server.py: handshakes (legacy, modern, auto), tools/list
// pagination, tools/call, tool errors, timeout + cancel, crash and restart with backoff, list_changed, shutdown.
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "mcp/client.h"
#include "test.h"

using namespace judas;
using namespace judas::mcp;

static ClientOptions opts(const std::vector<std::string> & server_args, const std::string & protocol = "auto") {
    ClientOptions o;
    o.name = "fake";
    o.argv = {"python3", "-I", std::string(JUDAS_TESTS_DIR) + "/fake_mcp_server.py"};
    for (auto & a : server_args) o.argv.push_back(a);
    o.env = {"PATH=/usr/local/bin:/usr/bin:/bin"};
    o.protocol = protocol;
    o.timeout_s = 5;
    o.startup_timeout_s = 10;
    o.probe_timeout_ms = 1500;
    o.shutdown_grace_ms = 500;
    o.backoff_initial_ms = 300;
    o.backoff_max_ms = 2000;
    return o;
}

static std::string text_of(const CallOutcome & c) {
    return c.ok && c.result.contains("content") ? c.result["content"][0].value("text", std::string()) : c.error;
}

int main() {
    // ---- legacy handshake (auto mode falls back after the discover probe fails) ----
    {
        Client c(opts({"--era", "legacy"}));
        std::string err;
        REQUIRE(c.start(err));
        CHECK(c.protocol_version() == "2025-11-25");
        CHECK(c.server_name() == "fake");
        CHECK(c.server_announces_list_changed());
        std::vector<ToolDef> tools;
        REQUIRE(c.list_tools(tools, err));
        CHECK(tools.size() == 9);
        CHECK(tools[0].name == "echo" && tools[0].input_schema["required"][0] == "text");
        CHECK(tools[0].annotations.value("readOnlyHint", false));
        auto r = c.call_tool("echo", {{"text", "hi"}});
        CHECK(r.ok && text_of(r) == "echo: hi");
        // tool-level error comes back as a normal result with isError
        r = c.call_tool("fail", json::object());
        CHECK(r.ok && r.result.value("isError", false) && text_of(r) == "boom");
        // protocol-level error (unknown tool) is a transport-clean error
        r = c.call_tool("nope", json::object());
        CHECK(!r.ok && r.error.find("-32602") != std::string::npos && r.error.find("Unknown tool") != std::string::npos);
    }
    // ---- modern (2026-07-28): discover, per-request _meta ----
    {
        Client c(opts({"--era", "modern"}));
        std::string err;
        REQUIRE(c.start(err));
        CHECK(c.protocol_version() == "2026-07-28");
        std::vector<ToolDef> tools;
        REQUIRE(c.list_tools(tools, err));
        CHECK(tools.size() == 9);
        auto r = c.call_tool("add", {{"a", 2}, {"b", 3}});
        CHECK(r.ok && text_of(r) == "5");
    }
    // ---- dual-era server: the modern path wins ----
    {
        Client c(opts({"--era", "both"}));
        std::string err;
        REQUIRE(c.start(err));
        CHECK(c.protocol_version() == "2026-07-28");
    }
    // ---- forced modes ----
    {
        Client c(opts({"--era", "legacy"}, "modern"));
        std::string err;
        CHECK(!c.start(err));
        CHECK(err.find("server/discover failed") != std::string::npos);
        CHECK(!c.running());
    }
    {
        Client c(opts({"--era", "modern"}, "legacy"));
        std::string err;
        CHECK(!c.start(err));
        CHECK(err.find("initialize failed") != std::string::npos);
    }
    // ---- a legacy server that never answers discover: the probe times out, then initialize works ----
    {
        auto o = opts({"--era", "legacy", "--silent-discover"});
        o.probe_timeout_ms = 600;
        Client c(o);
        std::string err;
        REQUIRE(c.start(err));
        CHECK(c.protocol_version() == "2025-11-25");
        CHECK(text_of(c.call_tool("echo", {{"text", "x"}})) == "echo: x");
    }
    // ---- pagination ----
    {
        Client c(opts({"--era", "legacy", "--pages", "2"}, "legacy"));
        std::string err;
        REQUIRE(c.start(err));
        std::vector<ToolDef> tools;
        REQUIRE(c.list_tools(tools, err));
        CHECK(tools.size() == 9);  // five pages
        CHECK(tools.back().name == "run");
    }
    // ---- timeout: the call is cancelled, the server stays usable ----
    {
        auto o = opts({"--era", "legacy"}, "legacy");
        o.timeout_s = 1;
        Client c(o);
        std::string err;
        REQUIRE(c.start(err));
        auto t0 = std::chrono::steady_clock::now();
        auto r = c.call_tool("slow", {{"seconds", 3}});
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!r.ok && r.error.find("timed out after 1s") != std::string::npos);
        CHECK(dt < 2.5);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // the slow reply arrives late and is dropped
        r = c.call_tool("echo", {{"text", "after"}});
        CHECK(r.ok && text_of(r) == "echo: after");
    }
    // ---- crash: the call fails with stderr, then the server restarts (after a backoff) on a later call ----
    {
        Client c(opts({"--era", "legacy"}, "legacy"));
        std::string err;
        REQUIRE(c.start(err));
        auto r = c.call_tool("pid", json::object());
        std::string pid1 = text_of(r);
        r = c.call_tool("crash", json::object());
        CHECK(!r.ok && r.error.find("crashed during the call") != std::string::npos);
        CHECK(r.error.find("crashing on purpose") != std::string::npos);
        CHECK(c.consecutive_failures() == 1);
        r = c.call_tool("echo", {{"text", "now"}});  // inside the backoff window
        CHECK(!r.ok && r.error.find("will be restarted") != std::string::npos);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        r = c.call_tool("pid", json::object());
        CHECK(r.ok);
        CHECK(text_of(r) != pid1 && !text_of(r).empty());
        CHECK(c.restarts() == 1);
        CHECK(c.consecutive_failures() == 0);
        // a second crash doubles the delay
        c.call_tool("crash", json::object());
        c.call_tool("crash", json::object());  // refused by backoff, does not count
        CHECK(c.consecutive_failures() == 1);
    }
    // ---- a server that dies while idle is restarted at once (no backoff) ----
    {
        Client c(opts({"--era", "legacy"}, "legacy"));
        std::string err;
        REQUIRE(c.start(err));
        pid_t pid = (pid_t) std::stol(text_of(c.call_tool("pid", json::object())));
        kill(pid, SIGKILL);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(!c.running());
        auto r = c.call_tool("echo", {{"text", "back"}});
        CHECK(r.ok && text_of(r) == "echo: back");
    }
    // ---- a command that cannot start ----
    {
        ClientOptions o = opts({});
        o.argv = {"/nonexistent/mcp-server"};
        Client c(o);
        std::string err;
        CHECK(!c.start(err));
        CHECK(err.find("cannot start") != std::string::npos);
        std::vector<ToolDef> t;
        CHECK(!c.list_tools(t, err));
        CHECK(err.find("down") != std::string::npos);  // backoff message
    }
    // ---- list_changed notification reaches the callback ----
    {
        Client c(opts({"--era", "legacy"}, "legacy"));
        std::atomic<int> changed{0};
        c.on_tools_changed([&] { changed++; });
        std::string err;
        REQUIRE(c.start(err));
        c.call_tool("grow", json::object());
        for (int i = 0; i < 50 && changed == 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(changed >= 1);
        std::vector<ToolDef> t;
        REQUIRE(c.list_tools(t, err));
        CHECK(t.back().name == "extra");
    }
    // ---- graceful shutdown: stdin closes, the server exits; stop() is idempotent and restart works ----
    {
        Client c(opts({"--era", "legacy"}, "legacy"));
        std::string err;
        REQUIRE(c.start(err));
        pid_t pid = (pid_t) std::stol(text_of(c.call_tool("pid", json::object())));
        auto t0 = std::chrono::steady_clock::now();
        c.stop();
        c.stop();
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 1.0);
        CHECK(kill(pid, 0) != 0);  // gone
        REQUIRE(c.start(err));
        CHECK(text_of(c.call_tool("echo", {{"text", "again"}})) == "echo: again");
        c.shutdown();
        CHECK(!c.call_tool("echo", {{"text", "x"}}).ok);
    }
    TEST_MAIN_END();
}
