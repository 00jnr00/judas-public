// Sandbox hardening: the fail-closed launch verdict (every case), the bwrap argv with network off by
// default, the one-line startup warning for sandbox "none", and the hard-deny floor on resolved paths.
#include <unistd.h>

#include <algorithm>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

#include "core/config.h"
#include "mcp/launch.h"
#include "tools/tools.h"
#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

// Run f with fd 2 redirected into a temp file and return what it wrote to stderr.
static std::string capture_stderr(const std::function<void()> & f) {
    std::FILE * t = std::tmpfile();
    int saved = dup(2);
    dup2(fileno(t), 2);
    f();
    std::fflush(nullptr);
    dup2(saved, 2);
    close(saved);
    rewind(t);
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, t)) > 0) out.append(buf, n);
    std::fclose(t);
    return out;
}

int main() {
    // ---- the verdict, a pure function: every combination ----
    using W = mcp::SandboxVerdict::What;
    McpServerConfig s;  // sandbox = true, network = false: the defaults
    s.name = "x";
    s.command = {"/bin/echo", "hi"};
    PermissionsConfig p;
    CHECK(mcp::sandbox_verdict(s, p, true).what == W::sandboxed);   // bwrap mode + present, wants it
    CHECK(mcp::sandbox_verdict(s, p, true).reason.empty());
    {
        mcp::SandboxVerdict v = mcp::sandbox_verdict(s, p, false);  // fails closed: should be sandboxed,
        CHECK(v.what == W::refuse);                                 // but bwrap is missing
        CHECK(v.reason.find("bwrap") != std::string::npos);
    }
    p.sandbox = "none";
    {
        mcp::SandboxVerdict v = mcp::sandbox_verdict(s, p, true);   // fails closed: no sandbox configured
        CHECK(v.what == W::refuse);
        CHECK(v.reason.find("permissions.sandbox is 'none'") != std::string::npos);
    }
    p.allow_unsandboxed_mcp = true;
    {
        mcp::SandboxVerdict v = mcp::sandbox_verdict(s, p, false);  // the explicit opt-in
        CHECK(v.what == W::unsandboxed);
        CHECK(v.reason.find("'none'") != std::string::npos && v.reason.find("allow_unsandboxed_mcp") != std::string::npos);
    }
    {
        mcp::SandboxVerdict v = mcp::sandbox_verdict(s, p, true);
        CHECK(v.what == W::unsandboxed);
        CHECK(v.reason.find("'none'") != std::string::npos && v.reason.find("allow_unsandboxed_mcp") != std::string::npos);
    }
    s.sandbox = false;  // the server opted out itself; allowed either way, flag or not
    CHECK(mcp::sandbox_verdict(s, PermissionsConfig{}, false).what == W::unsandboxed);
    CHECK(mcp::sandbox_verdict(s, p, false).reason.find("disabled") != std::string::npos);
    s.sandbox = true;

    // ---- build_launch: refuse leaves argv empty and logs why; the sandbox-"none" warning is one line ----
    // (this is the process's first sandbox-"none" launch, so the warning appears here exactly once)
    Config c;
    s.command = {"/bin/echo", "hi"};
    c.permissions.sandbox = "none";
    std::string err = capture_stderr([&] {
        auto l = mcp::build_launch(c, s);
        REQUIRE(l.refused);
        CHECK(!l.sandboxed && l.argv.empty());
        CHECK(l.note.rfind("refused: ", 0) == 0);
    });
    CHECK(err.find("mcp[x]: refusing to start") != std::string::npos);
    CHECK(err.find("permissions.sandbox is 'none'") != std::string::npos);
    CHECK(err.find("allow_unsandboxed_mcp = true to allow") != std::string::npos);
    CHECK(err.find("permissions.sandbox = \"none\"") != std::string::npos);  // the startup warning
    err = capture_stderr([&] { mcp::build_launch(c, s); });
    CHECK(err.find("refusing to start") != std::string::npos);
    CHECK(err.find("run unsandboxed") == std::string::npos);  // warned once, not once per server
    c.permissions.allow_unsandboxed_mcp = true;
    {
        auto l = mcp::build_launch(c, s);
        CHECK(!l.refused && !l.sandboxed && l.argv.size() == 2 && l.note.find("allow_unsandboxed_mcp") != std::string::npos);
    }

    // ---- the bwrap argv, and the per-server network switch ----
    if (fs::exists("/usr/bin/bwrap")) {
        Config c2;
        c2.permissions.sandbox = "bwrap";
        s.command = {"python3", "-I", "a.py"};
        s.network = false;
        auto l = mcp::build_launch(c2, s);
        CHECK(l.sandboxed && !l.refused && l.argv[0] == find_bwrap());
        auto has = [&](const std::string & a) { return std::find(l.argv.begin(), l.argv.end(), a) != l.argv.end(); };
        CHECK(has("--unshare-net"));  // network = false (the default): no network inside the sandbox
        CHECK(has("--unshare-pid") && has("--unshare-uts") && has("--unshare-ipc") && has("--die-with-parent"));
        CHECK(has("--ro-bind") && has("--tmpfs"));
        s.network = true;
        auto net = mcp::build_launch(c2, s);
        CHECK(std::find(net.argv.begin(), net.argv.end(), "--unshare-net") == net.argv.end());
        s.network = false;
    }

    // ---- the safety floor resolves paths before matching, and checks the given text too ----
    fs::path root = JUDAS_TEST_TMP "/judas-sandbox-test-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root / "open");
    fs::create_directories(root / ".ssh");
    {
        std::ofstream(root / "open" / "notes.txt") << "fine";
        std::ofstream(root / "open" / ".ssh" / "authorized_keys") << "key";
        std::ofstream(root / ".ssh" / "id_rsa") << "secret";
        std::ofstream(root / "secrets.env") << "K=1";
    }
    fs::create_symlink(root / ".ssh", root / "open" / "insidessh");
    fs::create_symlink(root / ".ssh" / "id_rsa", root / "closekey");
    fs::create_symlink(root / "secrets.env", root / "leak");
    ToolContext ctx;
    ctx.cwd = root.string();
    auto floor = [&](const std::string & path) { return judas::safety_floor_path(ctx, path); };
    // a symlink pointing at a secret-looking path: the resolved path is checked
    CHECK(!floor((root / "leak").string()).empty());                       // -> secrets.env
    CHECK(!floor((root / "closekey").string()).empty());                   // -> .ssh/id_rsa
    CHECK(!floor((root / "open" / "insidessh").string()).empty());         // -> the .ssh directory
    CHECK(!floor((root / "open" / "insidessh" / "authorized_keys").string()).empty());
    CHECK(!floor((root / "open" / "insidessh" / "notes.txt").string()).empty());  // any path through it
    // "a/../.ssh/id_rsa" spellings: refused even before resolution, since the given path is checked too
    CHECK(!floor((root / ".ssh").string()).empty());
    CHECK(!floor((root / ".ssh" / "id_rsa").string()).empty());
    CHECK(!floor("alpha/../.ssh/id_rsa").empty());
    CHECK(!floor(".ssh/authorized_keys").empty());
    // plain paths, with dotdot that never names a secret, still work
    CHECK(floor((root / "open" / "notes.txt").string()).empty());
    CHECK(floor("alpha/../open/notes.txt").empty());
    fs::remove_all(root);
    TEST_MAIN_END();
}
