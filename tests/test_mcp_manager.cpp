// Manager: config parsing, tool registration (prefix, allow/deny, schema pass-through), result conversion,
// the safety floor, approval, schema validation through the registry, role filters, sandbox argv.
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/config.h"
#include "mcp/filter.h"
#include "mcp/launch.h"
#include "mcp/manager.h"
#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

static std::string text_of(const json & r) { return r["content"][0].value("text", std::string()); }

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-mcp-mgr-test-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root);
    std::string fake = std::string(JUDAS_TESTS_DIR) + "/fake_mcp_server.py";

    // ---- config file: parse, defaults, dump round trip, validation ----
    {
        std::ofstream(root / "c.toml") << R"(
[permissions]
allow_unsandboxed_mcp = true
[mcp]
max_tools_per_agent = 5
[mcp.servers.fake]
command = ["python3", "-I", "x.py"]
env = { A = "1", B = "${HOME}" }
cwd = "~/w"
timeout_s = 7
tools_allow = ["ec*", "add"]
tools_deny = ["echo2"]
prefix = "f_"
network = true
writable = ["~/out"]
trusted = true
protocol = "legacy"
[mcp.servers.other]
command = ["true"]
enabled = false
)";
        setenv("JUDAS_CONFIG", (root / "c.toml").c_str(), 1);
        Config c = load_config((root / "noproj").string());
        unsetenv("JUDAS_CONFIG");
        CHECK(c.mcp.enabled && c.mcp.max_tools_per_agent == 5 && c.mcp.max_result_chars == 30000);
        REQUIRE(c.mcp.servers.size() == 2);
        auto & s = c.mcp.servers[0];
        CHECK(s.name == "fake" && s.command.size() == 3 && s.env.at("A") == "1" && s.timeout_s == 7);
        CHECK(s.prefix_set && s.prefix == "f_" && s.network && s.trusted && s.protocol == "legacy" && s.sandbox);
        CHECK(c.permissions.allow_unsandboxed_mcp);
        CHECK(s.tools_allow.size() == 2 && s.tools_deny[0] == "echo2" && s.writable[0] == "~/out");
        CHECK(!c.mcp.servers[1].enabled && !c.mcp.servers[1].prefix_set);
        CHECK(validate_config(c).empty() || validate_config(c)[0].find("mcp") == std::string::npos);
        std::string dump = dump_config(c);
        CHECK(dump.find("[mcp.servers.fake]") != std::string::npos && dump.find("prefix = 'f_'") != std::string::npos);
        CHECK(dump.find("[mcp.servers.other]") != std::string::npos && dump.find("prefix = 'other__'") != std::string::npos);
        CHECK(dump.find("allow_unsandboxed_mcp = true") != std::string::npos);

        // bad values are caught
        Config bad = c;
        bad.mcp.servers[0].command.clear();
        bad.mcp.servers[0].protocol = "x";
        bool cmd = false, proto = false;
        for (auto & p : validate_config(bad)) {
            if (p.find("mcp.servers.fake.command") != std::string::npos) cmd = true;
            if (p.find("protocol must be") != std::string::npos) proto = true;
        }
        CHECK(cmd && proto);
        // unknown key is an error with the key name
        std::ofstream(root / "bad.toml") << "[mcp.servers.x]\ncommand = [\"a\"]\nbogus = 1\n";
        setenv("JUDAS_CONFIG", (root / "bad.toml").c_str(), 1);
        bool threw = false;
        try { load_config((root / "noproj").string()); } catch (const std::exception & e) {
            threw = std::string(e.what()).find("mcp.servers.x.bogus") != std::string::npos;
        }
        unsetenv("JUDAS_CONFIG");
        CHECK(threw);

        // a project file cannot add MCP servers
        fs::create_directories(root / "proj/.judas");
        std::ofstream(root / "proj/.judas/config.toml") << "[mcp.servers.evil]\ncommand = [\"/bin/sh\"]\n[mcp]\nmax_tools_per_agent = 3\n";
        setenv("JUDAS_CONFIG", (root / "c.toml").c_str(), 1);
        Config p = load_config((root / "proj").string());
        unsetenv("JUDAS_CONFIG");
        CHECK(p.mcp.servers.size() == 2 && p.mcp.ignored_project_servers.size() == 1 && p.mcp.ignored_project_servers[0] == "evil");
        CHECK(p.mcp.max_tools_per_agent == 3);
    }

    // ---- the manager against the fake server ----
    Config cfg;
    cfg.permissions.allow_paths = {root.string()};
    cfg.permissions.mode = "auto-edit";
    cfg.permissions.sandbox = "none";          // the sandbox has its own test below
    cfg.permissions.allow_unsandboxed_mcp = true;  // sandbox "none" would refuse these servers otherwise
    cfg.data_dir = (root / "data").string();
    McpServerConfig sc;
    sc.name = "fake";
    sc.command = {"python3", "-I", fake, "--era", "legacy"};
    sc.protocol = "legacy";
    sc.timeout_s = 5;
    sc.tools_deny = {"crash", "slow", "grow"};
    cfg.mcp.servers.push_back(sc);
    McpServerConfig second = sc;
    second.name = "two";
    second.prefix = "t_";
    second.prefix_set = true;
    second.protocol = "auto";
    second.tools_allow = {"echo", "pid"};
    second.tools_deny = {};
    cfg.mcp.servers.push_back(second);
    McpServerConfig off = sc;
    off.name = "off";
    off.enabled = false;
    cfg.mcp.servers.push_back(off);

    ToolRegistry reg;
    register_builtin_tools(reg);
    size_t builtins = reg.list().size();
    {
        mcp::Manager mgr(cfg, reg);
        mgr.start();
        REQUIRE(mgr.wait_ready(20000));
        auto st = mgr.status();
        REQUIRE(st.size() == 3);
        CHECK(st[0].state == "ready" && st[0].tools_registered == 6 && st[0].tools_filtered == 3);
        CHECK(st[0].protocol == "2025-11-25" && !st[0].sandboxed);
        CHECK(st[1].state == "ready" && st[1].tools_registered == 2);
        CHECK(st[2].state == "disabled");

        const Tool * echo = reg.find("fake__echo");
        REQUIRE(echo != nullptr);
        CHECK(echo->source == "mcp:fake" && !echo->may_be_irreversible && !echo->writes);
        CHECK(echo->description == "Echo text back.");  // first paragraph only
        CHECK(echo->parameters["required"][0] == "text" && echo->parameters["properties"]["mode"]["enum"].size() == 2);
        CHECK(reg.find("t_echo") != nullptr && reg.find("fake__crash") == nullptr && reg.find("off__echo") == nullptr);
        CHECK(reg.find("fake__add")->may_be_irreversible);  // no readOnlyHint: treated as able to change things
        CHECK(reg.list().size() == builtins + 8);

        ToolContext ctx;
        ctx.cwd = root.string();
        ctx.cfg = &cfg;
        ctx.session = "sess1";
        ctx.agent = "a";

        // normal call
        auto r = reg.call("fake__echo", {{"text", "hello"}}, ctx);
        CHECK(r.ok && r.output == "echo: hello");
        // schema validation: the model gets the error, the server never sees the call
        r = reg.call("fake__echo", {{"text", "x"}, {"n", 0}}, ctx);
        CHECK(!r.ok && r.output.find("n: must be >= 1") != std::string::npos && r.output.find("Nothing ran") != std::string::npos);
        r = reg.call("fake__echo", {{"text", "x"}, {"mode", "zzz"}}, ctx);
        CHECK(!r.ok && r.output.find("must be one of") != std::string::npos);
        r = reg.call("fake__echo", {{"text", 5}}, ctx);
        CHECK(!r.ok && r.output.find("expected string, got integer") != std::string::npos);
        r = reg.call("fake__echo", {{"text", "x"}, {"extra", 1}}, ctx);  // additionalProperties:false
        CHECK(!r.ok && r.output.find("unknown argument 'extra'") != std::string::npos);
        // the same validator now guards built-in tools
        r = reg.call("read_file", {{"path", 7}}, ctx);
        CHECK(!r.ok && r.output.find("expected string") != std::string::npos);

        // tool-level error
        r = reg.call("fake__add", {{"a", 1}, {"b", 2}}, ctx);  // not readonly, auto-edit, no approver
        CHECK(!r.ok && r.output.find("not approved") != std::string::npos);
        bool asked = false;
        ctx.approve = [&](const std::string & what) { asked = what.find("fake/add") != std::string::npos; return true; };
        r = reg.call("fake__add", {{"a", 1}, {"b", 2}}, ctx);
        CHECK(asked && r.ok && r.output == "3" && r.irreversible);
        ctx.approve = [](const std::string &) { return false; };
        r = reg.call("fake__add", {{"a", 1}, {"b", 2}}, ctx);
        CHECK(!r.ok && r.output.find("declined") != std::string::npos);
        ctx.approve = nullptr;
        cfg.permissions.mode = "full-auto";
        r = reg.call("fake__add", {{"a", 4}, {"b", 2}}, ctx);
        CHECK(r.ok && r.output == "6");
        r = reg.call("fake__fail", json::object(), ctx);
        CHECK(!r.ok && r.output == "boom");

        // SAFETY FLOOR holds even in full-auto: commands and secret paths
        r = reg.call("fake__run", {{"command", "sudo rm -rf /tmp/x"}}, ctx);
        CHECK(!r.ok && r.output.find("privilege escalation") != std::string::npos);
        r = reg.call("fake__run", {{"command", "echo hi > /etc/passwd"}}, ctx);
        CHECK(!r.ok && r.output.find("auth database") != std::string::npos);
        r = reg.call("fake__run", {{"path", "/home/x/Documents/keys/service.env"}}, ctx);
        CHECK(!r.ok && r.output.find("refused") != std::string::npos);
        r = reg.call("fake__run", {{"path", "file:///some/dir/id_ed25519"}}, ctx);
        CHECK(!r.ok && r.output.find("secret file name") != std::string::npos);
        r = reg.call("fake__run", {{"command", "ls"}, {"path", (root / "ok.txt").string()}}, ctx);  // allowed path
        CHECK(r.ok && r.output == "ran");
        r = reg.call("fake__run", {{"path", "/usr/share/forbidden-write"}}, ctx);  // outside allow_paths, tool may write
        CHECK(!r.ok);
        // read-only tool: a path outside allow_paths is fine as far as the write policy goes, secrets still are not
        r = reg.call("t_echo", {{"text", "/usr/share/dict/words"}}, ctx);
        CHECK(r.ok);
        r = reg.call("t_echo", {{"text", "~/.ssh/x"}}, ctx);
        CHECK(!r.ok);
        cfg.permissions.mode = "auto-edit";

        // result conversion through a real call: image saved, resource inlined
        auto direct = mcp::convert_result(
            {{"content", {{{"type", "text"}, {"text", "here"}},
                          {{"type", "image"}, {"data", "iVBORw0KGgo="}, {"mimeType", "image/png"}},
                          {{"type", "resource"}, {"resource", {{"uri", "file:///x.txt"}, {"text", "inner"}}}},
                          {{"type", "resource_link"}, {"uri", "https://e/x"}, {"name", "X"}},
                          {{"type", "weird"}}}}},
            (root / "files").string(), "fake-image", 1000);
        CHECK(direct.ok);
        CHECK(direct.output.find("here\n[image: image/png, 8 bytes, saved to " + (root / "files").string()) == 0);
        CHECK(direct.output.find("[resource file:///x.txt]\ninner") != std::string::npos);
        CHECK(direct.output.find("[resource link: https://e/x (X)]") != std::string::npos);
        CHECK(direct.output.find("[unsupported content block: weird]") != std::string::npos);
        size_t png = 0;
        for (auto & e : fs::directory_iterator(root / "files")) { png += fs::file_size(e) == 8; }
        CHECK(png == 1);
        r = reg.call("t_pid", json::object(), ctx);
        CHECK(r.ok);
        auto big = mcp::convert_result({{"content", {{{"type", "text"}, {"text", std::string(5000, 'x')}}}}}, "", "t", 1000);
        CHECK(big.output.size() < 1100 && big.output.find("chars truncated") != std::string::npos);
        auto err = mcp::convert_result({{"isError", true}, {"content", json::array()}}, "", "t", 1000);
        CHECK(!err.ok && !err.output.empty());
        auto sc2 = mcp::convert_result({{"content", json::array()}, {"structuredContent", {{"k", 1}}}}, "", "t", 1000);
        CHECK(sc2.ok && sc2.output.find("\"k\": 1") != std::string::npos);
        CHECK(mcp::trim_description("a  b\n\nsecond", 100) == "a b");
        CHECK(mcp::trim_description(std::string(50, 'a') + " " + std::string(50, 'b'), 60) == std::string(50, 'a') + "...");
    }
    CHECK(reg.list().size() == builtins);  // the manager removed its tools on shutdown

    // ---- list_changed refreshes the registry while the manager runs ----
    {
        Config c2 = cfg;
        c2.permissions.mode = "full-auto";
        c2.mcp.servers.clear();
        McpServerConfig g = sc;
        g.tools_deny = {};
        c2.mcp.servers.push_back(g);
        ToolRegistry r2;
        mcp::Manager mgr(c2, r2);
        mgr.start();
        REQUIRE(mgr.wait_ready(20000));
        CHECK(r2.find("fake__extra") == nullptr);
        ToolContext ctx;
        ctx.cwd = root.string();
        ctx.cfg = &c2;
        CHECK(r2.call("fake__grow", json::object(), ctx).ok);
        for (int i = 0; i < 60 && !r2.find("fake__extra"); i++) usleep(50000);
        REQUIRE(r2.find("fake__extra") != nullptr);
        CHECK(r2.call("fake__extra", json::object(), ctx).output == "extra works");
        // a crash leaves the tools registered; the call reports it and a later call restarts the server
        auto r = r2.call("fake__crash", json::object(), ctx);
        CHECK(!r.ok && r.output.find("crashed") != std::string::npos);
        usleep(700000);
        CHECK(r2.call("fake__echo", {{"text", "up again"}}, ctx).output == "echo: up again");
        CHECK(mgr.status()[0].restarts == 1);
    }

    // ---- under bwrap: the server survives its connect thread (PDEATHSIG is per thread), and is confined ----
    if (fs::exists("/usr/bin/bwrap")) {
        Config c4 = cfg;
        c4.permissions.sandbox = "bwrap";
        c4.permissions.mode = "full-auto";
        c4.mcp.servers.clear();
        McpServerConfig g = sc;
        g.tools_deny = {};
        g.writable = {root.string()};
        c4.mcp.servers.push_back(g);
        ToolRegistry r4;
        mcp::Manager mgr(c4, r4);
        mgr.start();
        REQUIRE(mgr.wait_ready(20000));
        CHECK(mgr.status()[0].sandboxed);
        usleep(1200000);  // the connect thread is long gone
        ToolContext ctx;
        ctx.cwd = root.string();
        ctx.cfg = &c4;
        CHECK(r4.call("fake__echo", {{"text", "boxed"}}, ctx).output == "echo: boxed");
        CHECK(mgr.status()[0].restarts == 0);
    }

    // ---- a server that cannot start is reported, not fatal ----
    {
        Config c3 = cfg;
        c3.mcp.servers.clear();
        McpServerConfig b;
        b.name = "broken";
        b.command = {"/nonexistent/server"};
        c3.mcp.servers.push_back(b);
        ToolRegistry r3;
        mcp::Manager mgr(c3, r3);
        mgr.start();
        REQUIRE(mgr.wait_ready(20000));
        auto st = mgr.status();
        CHECK(st[0].state == "failed" && st[0].error.find("cannot start") != std::string::npos);
    }

    // ---- fail closed: a sandboxed-by-default server is refused when the sandbox is unavailable ----
    {
        Config c5 = cfg;
        c5.permissions.sandbox = "none";
        c5.permissions.allow_unsandboxed_mcp = false;  // the default: no unconfined MCP servers
        c5.mcp.servers.clear();
        c5.mcp.servers.push_back(sc);
        ToolRegistry r5;
        mcp::Manager mgr(c5, r5);
        mgr.start();
        REQUIRE(mgr.wait_ready(20000));
        auto st = mgr.status();
        CHECK(st[0].state == "failed");
        CHECK(!st[0].sandboxed && st[0].sandbox_note.find("refused") != std::string::npos);
        CHECK(r5.find("fake__echo") == nullptr);  // nothing was spawned, no tools registered
    }

    // ---- role filters ----
    {
        Tool b;
        b.name = "read_file";
        Tool m;
        m.name = "docs__convert";
        m.source = "mcp:docs";
        Tool m2;
        m2.name = "web__fetch";
        m2.source = "mcp:web";
        using mcp::role_allows;
        using V = std::vector<std::string>;  // two string literals in braces would pick the iterator-pair constructor
        CHECK(role_allows(V{}, b) && role_allows(V{}, m));
        CHECK(!role_allows(V{"read_file"}, m) && role_allows(V{"read_file"}, b));          // whitelist excludes MCP by default
        CHECK(role_allows(V{"read_file", "mcp:*"}, m) && role_allows(V{"read_file", "mcp"}, m2));
        CHECK(role_allows(V{"mcp:docs"}, m) && !role_allows(V{"mcp:docs"}, m2) && !role_allows(V{"mcp:docs"}, b));
        CHECK(role_allows(V{"docs__*"}, m) && !role_allows(V{"docs__*"}, m2));
        CHECK(role_allows(V{"docs__convert"}, m));
        CHECK(!role_allows(V{"!mcp:*"}, m) && role_allows(V{"!mcp:*"}, b));                 // exclusion-only: everything else
        CHECK(!role_allows(V{"!mcp:web"}, m2) && role_allows(V{"!mcp:web"}, m));
        CHECK(!role_allows(V{"mcp:*", "!web__*"}, m2) && role_allows(V{"mcp:*", "!web__*"}, m));
        CHECK(!role_allows(V{"!read_file"}, b));
    }

    // ---- launch: environment scrubbing, executable lookup, sandbox ----
    {
        setenv("JUDAS_TEST_SECRET", "hunter2", 1);
        setenv("JUDAS_TEST_PASS", "ok", 1);
        Config c;
        c.permissions.sandbox = "bwrap";
        McpServerConfig s;
        s.name = "x";
        s.command = {"python3", "-I", "a.py"};
        s.env["PASSED"] = "${JUDAS_TEST_PASS}";
        s.writable = {root.string()};
        s.cwd = root.string();
        auto l = mcp::build_launch(c, s);
        bool secret = false, passed = false;
        for (auto & e : l.env) {
            if (e.find("JUDAS_TEST_SECRET") != std::string::npos) secret = true;
            if (e == "PASSED=ok") passed = true;
        }
        CHECK(!secret && passed);
        if (fs::exists("/usr/bin/bwrap")) {
            CHECK(l.sandboxed && l.argv[0] == "/usr/bin/bwrap");
            auto has = [&](const std::string & a) { return std::find(l.argv.begin(), l.argv.end(), a) != l.argv.end(); };
            CHECK(has("--unshare-net") && has("--ro-bind") && has("--die-with-parent") && has("--chdir"));
            CHECK(l.argv[l.argv.size() - 3].find("python3") != std::string::npos);  // resolved executable
            s.network = true;
            auto net = mcp::build_launch(c, s);
            CHECK(std::find(net.argv.begin(), net.argv.end(), "--unshare-net") == net.argv.end());
        }
        s.sandbox = false;
        auto n = mcp::build_launch(c, s);
        CHECK(!n.sandboxed && n.argv.size() == 3 && n.cwd == root.string() && n.note.find("disabled") != std::string::npos);
        c.permissions.sandbox = "none";
        s.sandbox = true;
        CHECK(mcp::build_launch(c, s).refused);  // fail closed: sandbox "none" alone no longer runs servers
        c.permissions.allow_unsandboxed_mcp = true;
        auto none = mcp::build_launch(c, s);
        CHECK(!none.sandboxed && !none.refused && none.argv.size() == 3 &&
              none.note.find("allow_unsandboxed_mcp") != std::string::npos);
        CHECK(mcp::expand_value("~/x") == std::string(getenv("HOME")) + "/x");
        CHECK(mcp::expand_value("a${JUDAS_NOPE_UNSET}b") == "ab");
        CHECK(mcp::glob_match("do*__x?", "docs__xy") && !mcp::glob_match("a*", "ba"));
    }
    fs::remove_all(root);
    TEST_MAIN_END();
}
