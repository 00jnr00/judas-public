// task_list, task_create, task_update, task_delete: scheduled prompts, shared with the Tasks screen.
#include "tools/task_tools.h"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace judas {
namespace {

constexpr long long MIN_INTERVAL_S = 5 * 60;  // a runaway every-minute task would hog the one model

long long now_ms_() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string stamp(long long ms) {
    if (ms <= 0) return "-";
    std::time_t t = (std::time_t) (ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[40];
    std::strftime(buf, sizeof buf, "%a %Y-%m-%d %H:%M", &tm);
    return buf;
}

std::string every_text(long long s) {
    struct U { long long n; const char * one; const char * many; };
    for (U u : {U{86400, "day", "days"}, U{3600, "hour", "hours"}, U{60, "minute", "minutes"}})
        if (s > 0 && s % u.n == 0) {
            long long k = s / u.n;
            return k == 1 ? std::string("every ") + u.one : "every " + std::to_string(k) + " " + u.many;
        }
    return "every " + std::to_string(s) + " s";
}

std::string describe(const Store::TaskRow & t) {
    std::string sched = t.kind == "interval" ? every_text(t.interval_s)
                        : t.kind == "once"   ? "once at " + stamp(t.run_at_ms)
                                             : "manual (runs only when started by hand)";
    std::string s = "[" + t.id + "] " + (t.title.empty() ? "(untitled)" : t.title) + " | " + sched + " | " +
                    (t.enabled ? "enabled" : "paused");
    if (t.enabled && t.next_ms > 0) s += " | next " + stamp(t.next_ms);
    if (t.last_run_ms > 0) s += " | last run " + stamp(t.last_run_ms);
    if (!t.cwd.empty()) s += " | folder " + t.cwd;
    return s;
}

std::string clip(const std::string & s, size_t n) { return s.size() > n ? s.substr(0, n) + "..." : s; }

ToolResult refuse_autonomous(const ToolContext & ctx, const char * name) {
    if (ctx.autonomous) return {false, std::string(name) + ": refused, no user is present to approve a scheduled task"};
    return {true, ""};
}

ToolResult t_list(Store & store) {
    auto ts = store.list_tasks();
    if (ts.empty()) return {true, "no scheduled tasks"};
    std::string out;
    for (const auto & t : ts) {
        if (!out.empty()) out += '\n';
        out += describe(t) + "\n    prompt: " + clip(t.prompt, 300);
    }
    return {true, out};
}

ToolResult t_create(const json & a, Store & store, ToolContext & ctx) {
    if (auto r = refuse_autonomous(ctx, "task_create"); !r.ok) return r;
    std::string prompt = a.value("prompt", "");
    if (prompt.find_first_not_of(" \t\r\n") == std::string::npos)
        return {false, "task_create: prompt is empty; write the full instructions the task should run with"};
    std::string schedule = a.value("schedule", "repeat");
    long long now = now_ms_();
    Store::TaskRow t;
    t.prompt = prompt;
    t.title = a.value("title", "");
    if (t.title.empty()) t.title = clip(prompt, 60);
    t.cwd = a.value("folder", "");
    std::string at = a.value("at", "");
    long long at_ms = at.empty() ? 0 : parse_local_time(at, now);
    if (!at.empty() && at_ms == 0)
        return {false, "task_create: could not read at=\"" + at + "\"; use \"YYYY-MM-DD HH:MM\" or \"HH:MM\" (24 h, local time)"};

    if (schedule == "repeat" || schedule == "interval") {
        long long every = a.contains("every") && a["every"].is_number() ? (long long) a["every"].get<double>() : 1;
        std::string unit = a.value("unit", "days");
        long long mul = unit.rfind("min", 0) == 0 ? 60 : unit.rfind("hour", 0) == 0 ? 3600 : unit.rfind("day", 0) == 0 ? 86400 : 0;
        if (every < 1 || mul == 0) return {false, "task_create: repeat needs every >= 1 and unit minutes, hours or days"};
        t.kind = "interval";
        t.interval_s = every * mul;
        if (t.interval_s < MIN_INTERVAL_S) return {false, "task_create: repeat at most every 5 minutes"};
        long long first = at_ms > 0 ? at_ms : now + t.interval_s * 1000;
        while (first <= now) first += t.interval_s * 1000;
        t.run_at_ms = first;
        t.next_ms = first;
    } else if (schedule == "once") {
        if (at_ms == 0) return {false, "task_create: once needs at (\"YYYY-MM-DD HH:MM\" or \"HH:MM\")"};
        if (at_ms <= now) return {false, "task_create: that time is in the past"};
        t.kind = "once";
        t.run_at_ms = at_ms;
        t.next_ms = at_ms;
    } else if (schedule == "manual") {
        t.kind = "manual";
    } else {
        return {false, "task_create: schedule must be repeat, once or manual"};
    }

    std::string when = t.kind == "interval" ? every_text(t.interval_s) + ", first run " + stamp(t.next_ms)
                       : t.kind == "once"   ? "once at " + stamp(t.run_at_ms)
                                            : "manual";
    std::string what = "Schedule a task \"" + t.title + "\" (" + when + ") that runs this prompt as a new session:\n" +
                       clip(prompt, 600);
    if (!ctx.approve_strict || !ctx.approve_strict(what)) return {false, "task_create: the user declined (or no one could approve)"};
    auto made = store.create_task(t);
    return {true, "scheduled: " + describe(made)};
}

ToolResult t_update(const json & a, Store & store, ToolContext & ctx) {
    if (auto r = refuse_autonomous(ctx, "task_update"); !r.ok) return r;
    auto t = store.get_task(a.value("id", ""));
    if (!t) return {false, "task_update: no task with that id (task_list shows them)"};
    if (!a.contains("enabled") || !a["enabled"].is_boolean()) return {false, "task_update: pass enabled true or false"};
    bool on = a["enabled"].get<bool>();
    if (on == t->enabled) return {true, "unchanged: " + describe(*t)};
    if (!ctx.approve_strict || !ctx.approve_strict((on ? "Resume" : "Pause") + std::string(" the task \"") + t->title + "\""))
        return {false, "task_update: the user declined (or no one could approve)"};
    t->enabled = on;
    if (on && t->kind == "interval") {
        long long now = now_ms_(), step = t->interval_s * 1000, next = t->next_ms > 0 ? t->next_ms : now + step;
        while (next <= now) next += step;
        t->next_ms = next;
    }
    store.update_task(*t);
    return {true, "updated: " + describe(*t)};
}

ToolResult t_delete(const json & a, Store & store, ToolContext & ctx) {
    if (auto r = refuse_autonomous(ctx, "task_delete"); !r.ok) return r;
    auto t = store.get_task(a.value("id", ""));
    if (!t) return {false, "task_delete: no task with that id (task_list shows them)"};
    if (!ctx.approve_strict || !ctx.approve_strict("Delete the task \"" + t->title + "\" (sessions it made are kept)"))
        return {false, "task_delete: the user declined (or no one could approve)"};
    store.delete_task(t->id);
    return {true, "deleted task " + t->id};
}

}  // namespace

long long parse_local_time(const std::string & s, long long now_ms) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    std::tm tm{};
    if (std::sscanf(s.c_str(), "%d-%d-%d%*[ T]%d:%d", &y, &mo, &d, &h, &mi) == 5) {
        tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    } else if (std::sscanf(s.c_str(), "%d:%d", &h, &mi) == 2 && s.find('-') == std::string::npos) {
        y = 0;  // the failed date scan above may have stored the hour here
        std::time_t now = (std::time_t) (now_ms / 1000);
        localtime_r(&now, &tm);
    } else {
        return 0;
    }
    if (h < 0 || h > 23 || mi < 0 || mi > 59) return 0;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = 0; tm.tm_isdst = -1;
    std::time_t t = std::mktime(&tm);
    if (t == (std::time_t) -1) return 0;
    long long ms = (long long) t * 1000;
    if (y == 0 && ms <= now_ms) {  // "HH:MM" already passed today: tomorrow
        tm.tm_mday += 1; tm.tm_isdst = -1;
        ms = (long long) std::mktime(&tm) * 1000;
    }
    return ms;
}

void register_task_tools(ToolRegistry & reg, Store & store) {
    {
        Tool t;
        t.name = "task_list";
        t.description = "List the scheduled tasks (saved prompts Judas runs on its own as new sessions): id, title, "
                        "schedule, paused or enabled, next and last run, and the prompt.";
        t.parameters = {{"type", "object"}, {"properties", json::object()}};
        t.run = [&store](const json &, ToolContext &) { return t_list(store); };
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "task_create";
        t.description =
            "Schedule a task: a prompt Judas runs by itself later as a new session (the user approves it first). "
            "Write the prompt as complete standalone instructions, since the run has no memory of this chat. "
            "schedule repeat (every N minutes/hours/days, first run at `at`), once (at `at`) or manual. "
            "Times are local, \"YYYY-MM-DD HH:MM\" or \"HH:MM\" (next time that clock time comes). Tasks only run "
            "while Judas is running; missed repeats are skipped.";
        t.parameters = {
            {"type", "object"},
            {"properties",
             {{"title", {{"type", "string"}, {"description", "Short name shown in the Tasks list"}}},
              {"prompt", {{"type", "string"}, {"description", "Full instructions the task runs with"}}},
              {"schedule", {{"type", "string"}, {"enum", {"repeat", "once", "manual"}}}},
              {"every", {{"type", "integer"}, {"minimum", 1}, {"description", "repeat: how many units between runs"}}},
              {"unit", {{"type", "string"}, {"enum", {"minutes", "hours", "days"}}}},
              {"at", {{"type", "string"}, {"description", "once: when to run. repeat: first run, which sets the time of day"}}},
              {"folder", {{"type", "string"}, {"description", "Optional working folder for the run"}}}}},
            {"required", {"prompt", "schedule"}}};
        t.run = [&store](const json & a, ToolContext & ctx) { return t_create(a, store, ctx); };
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "task_update";
        t.description = "Pause (enabled false) or resume (enabled true) a scheduled task by id. The user approves it.";
        t.parameters = {{"type", "object"},
                        {"properties", {{"id", {{"type", "string"}}}, {"enabled", {{"type", "boolean"}}}}},
                        {"required", {"id", "enabled"}}};
        t.run = [&store](const json & a, ToolContext & ctx) { return t_update(a, store, ctx); };
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "task_delete";
        t.description = "Delete a scheduled task by id. The user approves it. Sessions it already made are kept.";
        t.parameters = {{"type", "object"}, {"properties", {{"id", {{"type", "string"}}}}}, {"required", {"id"}}};
        t.run = [&store](const json & a, ToolContext & ctx) { return t_delete(a, store, ctx); };
        reg.add(std::move(t));
    }
}

}  // namespace judas
