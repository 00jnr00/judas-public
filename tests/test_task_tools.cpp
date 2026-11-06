// Scheduled-task tools: local time parsing, create (repeat / once / manual), approval gate, pause, delete.
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>

#include "store/store.h"
#include "test.h"
#include "tools/task_tools.h"
#include "tools/tools.h"

using namespace judas;
namespace fs = std::filesystem;

static long long t_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static std::tm local(long long ms) {
    std::time_t t = (std::time_t) (ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    return tm;
}

static void test_parse() {
    long long now = t_now();
    long long a = parse_local_time("2031-03-04 07:30", now);
    std::tm tm = local(a);
    CHECK(tm.tm_year == 131 && tm.tm_mon == 2 && tm.tm_mday == 4 && tm.tm_hour == 7 && tm.tm_min == 30);
    CHECK(parse_local_time("2031-03-04T07:30", now) == a);
    long long b = parse_local_time("08:00", now);
    CHECK(b > now && b - now <= 24LL * 3600 * 1000 + 3600 * 1000);
    CHECK(local(b).tm_hour == 8 && local(b).tm_min == 0);
    CHECK(parse_local_time("tomorrow", now) == 0);
    CHECK(parse_local_time("25:00", now) == 0);
}

static void test_tools(const std::string & db) {
    Store store(db);
    ToolRegistry reg;
    register_task_tools(reg, store);
    ToolContext ctx;
    std::string asked;
    bool yes = false;
    ctx.approve_strict = [&](const std::string & what) { asked = what; return yes; };

    // declined: nothing saved
    auto r = reg.call("task_create", {{"prompt", "Summarize my plan"}, {"schedule", "repeat"}, {"every", 1}, {"unit", "days"}, {"at", "08:00"}}, ctx);
    CHECK(!r.ok && store.list_tasks().empty());
    CHECK(asked.find("every day") != std::string::npos);

    yes = true;
    r = reg.call("task_create", {{"title", "Morning"}, {"prompt", "Summarize my plan"}, {"schedule", "repeat"}, {"every", 1}, {"unit", "days"}, {"at", "08:00"}}, ctx);
    CHECK(r.ok);
    auto ts = store.list_tasks();
    REQUIRE(ts.size() == 1);
    CHECK(ts[0].kind == "interval" && ts[0].interval_s == 86400 && ts[0].enabled);
    CHECK(ts[0].next_ms > t_now() && local(ts[0].next_ms).tm_hour == 8 && local(ts[0].next_ms).tm_min == 0);

    // too frequent, past once, bad time
    CHECK(!reg.call("task_create", {{"prompt", "x"}, {"schedule", "repeat"}, {"every", 1}, {"unit", "minutes"}}, ctx).ok);
    CHECK(!reg.call("task_create", {{"prompt", "x"}, {"schedule", "once"}, {"at", "2001-01-01 10:00"}}, ctx).ok);
    CHECK(!reg.call("task_create", {{"prompt", "x"}, {"schedule", "once"}, {"at", "soon"}}, ctx).ok);
    CHECK(reg.call("task_create", {{"prompt", "Check the build"}, {"schedule", "manual"}}, ctx).ok);
    CHECK(store.list_tasks().size() == 2);

    auto list = reg.call("task_list", json::object(), ctx);
    CHECK(list.ok && list.output.find("Morning") != std::string::npos && list.output.find("every day") != std::string::npos);

    std::string id = ts[0].id;
    CHECK(reg.call("task_update", {{"id", id}, {"enabled", false}}, ctx).ok);
    CHECK(!store.get_task(id)->enabled);
    CHECK(reg.call("task_update", {{"id", id}, {"enabled", true}}, ctx).ok);
    CHECK(store.get_task(id)->enabled && store.get_task(id)->next_ms > t_now());
    CHECK(reg.call("task_delete", {{"id", id}}, ctx).ok);
    CHECK(!store.get_task(id));

    // no user present: refused before asking
    ToolContext bg;
    bg.autonomous = true;
    bg.approve_strict = [](const std::string &) { return true; };
    CHECK(!reg.call("task_create", {{"prompt", "x"}, {"schedule", "manual"}}, bg).ok);
    // no approver: denied
    ToolContext none;
    CHECK(!reg.call("task_create", {{"prompt", "x"}, {"schedule", "manual"}}, none).ok);
}

int main() {
    fs::path root = fs::path("/tmp/opencode") / ("judas-task-tools-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    test_parse();
    test_tools((root / "t.db").string());
    fs::remove_all(root);
    std::printf("test_task_tools: done\n");
    TEST_MAIN_END();
}
