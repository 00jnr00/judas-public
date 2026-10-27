// Hooks: stdin delivery, blocking semantics, match filter, timeout kill.
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <sstream>

#include "hooks/hooks.h"
#include "test.h"

using namespace judas;

static std::string slurp(const std::string & p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static bool exists(const std::string & p) { return access(p.c_str(), F_OK) == 0; }

static Hook mk(const std::string & ev, const std::string & cmd, bool blocking, int to = 5, const std::string & match = "") {
    Hook h;
    h.event = ev; h.command = cmd; h.blocking = blocking; h.timeout_s = to; h.match = match;
    return h;
}
static Hooks make(std::vector<Hook> hs, bool enabled = true) {
    HooksConfig c;
    c.enabled = enabled;
    c.hooks = std::move(hs);
    return Hooks(c);
}

int main() {
    std::string dir = JUDAS_TEST_TMP "/judas-hooks-test-" + std::to_string(getpid());
    system(("mkdir -p " + dir).c_str());

    // disabled: always allow, nothing runs
    {
        Hooks h = make({mk("pre-edit", "touch " + dir + "/ran; exit 3", true)}, false);
        auto o = h.run("pre-edit", "x", json::object());
        CHECK(o.allow);
        CHECK(!exists(dir + "/ran"));
    }
    // stdin delivery + event env
    {
        std::string f = dir + "/stdin.json", e = dir + "/event.txt";
        Hooks h = make({mk("post-tool", "cat > " + f + "; echo $JUDAS_HOOK_EVENT > " + e, false)});
        json p = {{"tool", "bash"}, {"ok", true}};
        auto o = h.run("post-tool", "bash", p);
        CHECK(o.allow);
        CHECK(slurp(f) == p.dump());
        CHECK(slurp(e) == "post-tool\n");
        // other events do not fire it
        std::remove(f.c_str());
        h.run("pre-tool", "bash", p);
        CHECK(!exists(f));
    }
    // blocking vs non-blocking pre-edit exit 3
    {
        auto o = make({mk("pre-edit", "echo nope >&2; exit 3", true)}).run("pre-edit", "a", json::object());
        CHECK(!o.allow);
        CHECK(o.reason == "nope");
        auto q = make({mk("pre-edit", "exit 3", true)}).run("pre-edit", "a", json::object());
        CHECK(!q.allow);
        CHECK(q.reason == "hook pre-edit blocked (exit 3)");
        CHECK(make({mk("pre-edit", "exit 3", false)}).run("pre-edit", "a", json::object()).allow);
        // blocking non-pre event does not block
        CHECK(make({mk("post-edit", "exit 3", true)}).run("post-edit", "a", json::object()).allow);
    }
    // blocking exit 0 allows; a block stops later hooks
    {
        CHECK(make({mk("pre-tool", "exit 0", true)}).run("pre-tool", "bash", json::object()).allow);
        std::string f = dir + "/later";
        auto o = make({mk("pre-tool", "exit 1", true), mk("pre-tool", "touch " + f, false)})
                     .run("pre-tool", "bash", json::object());
        CHECK(!o.allow);
        CHECK(!exists(f));
    }
    // match filter
    {
        std::string f = dir + "/matched";
        Hooks h = make({mk("post-edit", "touch " + f, false, 5, "secret")});
        h.run("post-edit", "/home/x/readme", json::object());
        CHECK(!exists(f));
        h.run("post-edit", "/home/x/secret.txt", json::object());
        CHECK(exists(f));
    }
    // timeout kills the process group
    {
        auto t0 = std::chrono::steady_clock::now();
        auto o = make({mk("pre-tool", "sleep 30", true, 1)}).run("pre-tool", "bash", json::object());
        double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(!o.allow);
        CHECK(dt < 5.0);
        CHECK(o.reason.find("timed out") != std::string::npos);
    }
    // large payload larger than the pipe buffer is delivered intact
    {
        std::string f = dir + "/big.json";
        json p = {{"blob", std::string(300000, 'x')}};
        make({mk("turn-end", "cat > " + f, false)}).run("turn-end", "", p);
        CHECK(slurp(f) == p.dump());
    }
    system(("rm -rf " + dir).c_str());
    TEST_MAIN_END();
}
