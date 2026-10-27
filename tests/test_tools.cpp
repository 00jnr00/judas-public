#include <cstdlib>
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "test.h"
#include "tools/tools.h"
#include <arpa/inet.h>
#include <sys/socket.h>

using namespace judas;
namespace fs = std::filesystem;

static std::string slurp(const fs::path & p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static void put(const fs::path & p, const std::string & s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-tools-test-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root / "work");
    Config cfg;
    cfg.permissions.allow_paths = {root.string()};
    cfg.permissions.mode = "auto-edit";
    cfg.permissions.deny_commands = {"rm -rf /"};
    Journal journal(root / "journal");
    ToolRegistry reg;
    register_builtin_tools(reg);
    CHECK(reg.list().size() == 12);

    ToolContext ctx;
    ctx.cwd = (root / "work").string();
    ctx.cfg = &cfg;
    ctx.journal = &journal;
    ctx.session = "s1";
    ctx.agent = "t";

    // unknown tool, missing args
    auto r = reg.call("nope", json::object(), ctx);
    CHECK(!r.ok && r.output.find("read_file") != std::string::npos);
    r = reg.call("write_file", {{"path", "a.txt"}}, ctx);
    CHECK(!r.ok && r.output.find("content") != std::string::npos);

    // write / read / edit round trip, journaled
    ctx.checkpoint = journal.begin("s1", "t", "write_file a.txt");
    put(root / "work/a.txt", "orig\n");
    r = reg.call("write_file", {{"path", "a.txt"}, {"content", "hello\nworld\nhello\n"}}, ctx);
    CHECK(r.ok);
    journal.commit(ctx.checkpoint);
    CHECK(slurp(root / "work/a.txt") == "hello\nworld\nhello\n");
    r = reg.call("read_file", {{"path", "a.txt"}}, ctx);
    CHECK(r.ok && r.output.find("     2\tworld") != std::string::npos && r.output.find("3 lines total") != std::string::npos);
    r = reg.call("edit_file", {{"path", "a.txt"}, {"old_string", "hello"}, {"new_string", "x"}}, ctx);
    CHECK(!r.ok && r.output.find("2 times") != std::string::npos);
    r = reg.call("edit_file", {{"path", "a.txt"}, {"old_string", "nothere"}, {"new_string", "x"}}, ctx);
    CHECK(!r.ok);
    r = reg.call("edit_file", {{"path", "a.txt"}, {"old_string", "world"}, {"new_string", "there"}}, ctx);
    CHECK(r.ok && slurp(root / "work/a.txt") == "hello\nthere\nhello\n");
    r = reg.call("edit_file", {{"path", "a.txt"}, {"old_string", "hello"}, {"new_string", "x"}, {"replace_all", true}}, ctx);
    CHECK(r.ok && slurp(root / "work/a.txt") == "x\nthere\nx\n");

    // snapshot revert restores the original
    auto cps = journal.list("s1");
    REQUIRE(!cps.empty());
    r = reg.call("snapshot_list", json::object(), ctx);
    CHECK(r.ok && r.output.find(cps[0].id) != std::string::npos);
    r = reg.call("snapshot_diff", {{"id", cps[0].id}}, ctx);
    CHECK(r.ok);
    r = reg.call("snapshot_revert", {{"id", cps[0].id}}, ctx);
    CHECK(r.ok);
    CHECK(slurp(root / "work/a.txt") == "orig\n");

    // new file in nested dir
    r = reg.call("write_file", {{"path", "sub/dir/b.cpp"}, {"content", "int main() { return 42; }\n"}}, ctx);
    CHECK(r.ok && fs::exists(root / "work/sub/dir/b.cpp"));

    // deny paths
    r = reg.call("write_file", {{"path", ".env"}, {"content", "X=1"}}, ctx);
    CHECK(!r.ok && !fs::exists(root / "work/.env"));
    put(root / "work/k.env", "S=1");
    r = reg.call("read_file", {{"path", "k.env"}}, ctx);
    CHECK(!r.ok);
    cfg.permissions.secret_paths = {"~/.ssh", "*/SECRET STUFF/*", (root / "vault").string()};
    r = reg.call("write_file", {{"path", (root / "SECRET STUFF/x.txt").string()}, {"content", "x"}}, ctx);
    CHECK(!r.ok && !fs::exists(root / "SECRET STUFF"));
    put(root / "vault/k.txt", "k");
    r = reg.call("read_file", {{"path", (root / "vault/k.txt").string()}}, ctx);
    CHECK(!r.ok && r.output.find("secret_paths") != std::string::npos);
    cfg.permissions.secret_paths = {"~/.ssh", "~/.gnupg"};
    r = reg.call("write_file", {{"path", JUDAS_TEST_TMP "/outside-judas.txt"}, {"content", "x"}}, ctx);
    CHECK(!r.ok && r.output.find("allowed") != std::string::npos);
    cfg.permissions.deny_paths = {(root / "work/locked").string()};
    r = reg.call("write_file", {{"path", "locked/deep/f.txt"}, {"content", "x"}}, ctx);
    CHECK(!r.ok);
    cfg.permissions.deny_paths.clear();

    // shell
    cfg.permissions.mode = "full-auto";
    r = reg.call("shell", {{"command", "pwd; echo err 1>&2; exit 3"}}, ctx);
    CHECK(!r.ok && r.irreversible && r.output.find("/work") != std::string::npos && r.output.find("err") != std::string::npos &&
          r.output.find("exit code 3") != std::string::npos);
    r = reg.call("shell", {{"command", "echo ok"}}, ctx);
    CHECK(r.ok && r.output.find("ok") != std::string::npos);
    r = reg.call("shell", {{"command", "rm -rf / --no-preserve-root"}}, ctx);
    CHECK(!r.ok && r.output.find("refused") != std::string::npos);
    cfg.permissions.mode = "auto-edit";
    r = reg.call("shell", {{"command", "echo hi"}}, ctx);
    CHECK(!r.ok && r.output.find("not approved") != std::string::npos);
    ctx.approve = [](const std::string &) { return true; };
    r = reg.call("shell", {{"command", "echo hi"}}, ctx);
    CHECK(r.ok);
    ctx.approve = [](const std::string &) { return false; };
    r = reg.call("shell", {{"command", "echo hi"}}, ctx);
    CHECK(!r.ok);
    ctx.approve = nullptr;
    cfg.permissions.mode = "suggest";
    r = reg.call("write_file", {{"path", "s.txt"}, {"content", "x"}}, ctx);
    CHECK(!r.ok && !fs::exists(root / "work/s.txt"));
    cfg.permissions.mode = "full-auto";
    // timeout kills the process group (child sleep too)
    auto t0 = std::chrono::steady_clock::now();
    r = reg.call("shell", {{"command", "sleep 30 & sleep 30"}, {"timeout_s", 1}}, ctx);
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(!r.ok && r.output.find("timed out") != std::string::npos && dt < 5);
    // big output is capped
    r = reg.call("shell", {{"command", "head -c 100000 /dev/zero | tr '\\0' a"}}, ctx);
    CHECK(r.ok && r.output.size() < 31000 && r.output.find("truncated") != std::string::npos);

    // glob / grep / list_dir / binary
    put(root / "work/src/one.cpp", "void alpha() {}\nvoid Beta() {}\n");
    put(root / "work/src/two.h", "// alpha again\n");
    put(root / "work/node_modules/x.cpp", "alpha\n");
    r = reg.call("glob", {{"pattern", "**/*.cpp"}}, ctx);
    CHECK(r.ok && r.output.find("one.cpp") != std::string::npos && r.output.find("b.cpp") != std::string::npos &&
          r.output.find("node_modules") == std::string::npos && r.output.find("two.h") == std::string::npos);
    r = reg.call("glob", {{"pattern", "src/*.h"}}, ctx);
    CHECK(r.ok && r.output.find("two.h") != std::string::npos);
    r = reg.call("grep", {{"pattern", "alpha"}}, ctx);
    CHECK(r.ok && r.output.find("one.cpp:1:void alpha") != std::string::npos && r.output.find("two.h:1:") != std::string::npos);
    r = reg.call("grep", {{"pattern", "beta"}, {"ignore_case", true}, {"glob", "*.cpp"}}, ctx);
    CHECK(r.ok && r.output.find("one.cpp:2:") != std::string::npos);
    r = reg.call("grep", {{"pattern", "zzzzz"}}, ctx);
    CHECK(r.ok && r.output.find("no matches") != std::string::npos);
    r = reg.call("list_dir", {{"depth", 3}}, ctx);
    CHECK(r.ok && r.output.find("src/") != std::string::npos && r.output.find("node_modules") == std::string::npos);
    { std::ofstream f(root / "work/bin.dat", std::ios::binary); f.write("ab\0cd", 5); }
    r = reg.call("read_file", {{"path", "bin.dat"}}, ctx);
    CHECK(!r.ok && r.output.find("binary") != std::string::npos);
    std::string many;
    for (int i = 1; i <= 50; i++) many += "line" + std::to_string(i) + "\n";
    put(root / "work/many.txt", many);
    r = reg.call("read_file", {{"path", "many.txt"}, {"offset", 10}, {"limit", 3}}, ctx);
    CHECK(r.ok && r.output.find("line10") != std::string::npos && r.output.find("line13") == std::string::npos &&
          r.output.find("50 lines total") != std::string::npos);
    // a big file comes back in a bounded part with a pointer to the rest, not all 2000 lines
    std::string wide;
    for (int i = 1; i <= 2000; i++) wide += "w" + std::to_string(i) + std::string(120, '.') + "\n";
    put(root / "work/wide.txt", wide);
    r = reg.call("read_file", {{"path", "wide.txt"}}, ctx);
    CHECK(r.ok && r.output.size() < 31000 && r.output.find("use offset/limit") != std::string::npos &&
          r.output.find("w1.") != std::string::npos);

    // web (skipped when SearXNG is down)
    {
        // plain TCP probe for SearXNG
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(8888);
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        // CI sets JUDAS_TESTS_NO_NET: a live search on every commit got SearXNG rate-limited upstream.
        bool up = !std::getenv("JUDAS_TESTS_NO_NET") && connect(fd, (sockaddr *) &sa, sizeof sa) == 0;
        close(fd);
        if (up) {
            r = reg.call("web_search", {{"query", "llama.cpp github"}, {"max_results", 3}}, ctx);
            CHECK(r.ok && r.output.find("http") != std::string::npos);
        } else {
            std::fprintf(stderr, "SearXNG down, web tests skipped\n");
        }
        Config bad = cfg;
        bad.search.searxng = "http://127.0.0.1:1";
        ToolContext c2 = ctx;
        c2.cfg = &bad;
        r = reg.call("web_search", {{"query", "x"}}, c2);
        CHECK(!r.ok && r.output.find("not reachable") != std::string::npos);
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
