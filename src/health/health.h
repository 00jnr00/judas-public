// Judas Health Link, desktop side. The phone (Judas Android app) answers read-only questions about Health Connect
// data over the tailnet; this module is the client, the week-vs-normal arithmetic, the weekly check-in and the
// health_query tool. Health data is SILENT SECRET: it is never written to memory, skills or logs, and a session
// that reads it is tainted (see tools/tools.h health_taint_refusal). No scores, grades, streaks or goals.
#pragma once

#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "tools/tools.h"

namespace judas::health {

// ---- phone client ----------------------------------------------------------------------------------------------
struct Reply {
    int status = 0;            // HTTP status, 0 when the phone did not answer
    json body;                 // parsed JSON (null when the body was not JSON)
    std::string transport;     // non-empty when there was no HTTP answer: "unreachable" | "timeout" | "no token" | "bad url"
};
// GET url + path_query with the bearer token read from the env var named by cfg.token_env.
Reply phone_get(const HealthConfig & cfg, const std::string & path_query);
// "" when the reply is a 200, else a plain sentence for the model or the log (no health values).
std::string failure_text(const Reply & r, const std::string & metric, const HealthConfig & cfg);
// True when the URL's host is on this machine or a private/tailnet network (health data may be handled there).
bool url_is_local(const std::string & url);

// ---- date helpers (local dates, YYYY-MM-DD) ----------------------------------------------------------------------
std::string date_of(std::time_t t);                   // local date of t
std::string add_days(const std::string & date, int n);
bool valid_date(const std::string & d);
int weekday_of(const std::string & date);             // 0 = Sunday .. 6 = Saturday
int weekday_from_name(const std::string & name);      // "sun" -> 0 .. "sat" -> 6, -1 if unknown

// ---- week vs normal (computed in code, never by the model) -------------------------------------------------------
struct Stat {
    int week_n = 0, normal_n = 0;          // days with data in the last 7 days / the prior 28 days
    double week_avg = 0, normal_avg = 0;   // minutes asleep, bpm, or ms
};
struct Baseline {
    std::string week_from, week_to, normal_from, normal_to;
    Stat sleep, rhr, hrv;
};
// Rows are the phone's day rows. Week = today-6..today; normal = today-34..today-7.
Baseline compute_baseline(const json & sleep_rows, const json & rhr_rows, const json & hrv_rows, const std::string & today);
// Fetches the three metrics (one request each). On failure returns nullopt and sets err (a plain sentence). A metric the
// phone refuses with 403 is left empty and noted in `notes`.
std::optional<Baseline> fetch_baseline(const HealthConfig & cfg, const std::string & today, std::string & err,
                                       std::vector<std::string> * notes = nullptr);
// The tool's JSON for metric "week_vs_normal": averages, deltas and day counts. No verdicts.
json baseline_json(const Baseline & b);

// Check-in thresholds: at least 4 days this week and 14 in the normal window for the metric, and then sleep avg 60 min
// or more under normal, resting HR avg 5 bpm or more over, HRV avg 15% or more under.
struct Triggers { bool sleep = false, rhr = false, hrv = false; bool any() const { return sleep || rhr || hrv; } };
Triggers check_triggers(const Baseline & b);
// "5h40" (minutes -> h + two-digit minutes); under an hour "45 min".
std::string format_duration(int minutes);
// The one message: plain facts for the triggered metrics plus one question. "" when nothing triggered.
std::string checkin_message(const Baseline & b);

// ---- weekly check-in ---------------------------------------------------------------------------------------------
// "HH:MM-HH:MM" local, crossing midnight allowed; empty = never quiet.
bool in_quiet(const std::string & spec, const std::tm & now);
// Pure schedule decision. last_run is the date (YYYY-MM-DD) of the last run that finished; never due on Saturday.
bool weekly_due(const HealthConfig & cfg, bool toggle_on, const std::tm & now, const std::string & last_run, bool quiet);

struct WeeklyEnv {
    std::string data_dir;                                   // state file lives here: health-state.json
    std::string quiet_hours;                                // heartbeat.quiet_hours
    std::function<bool(const std::string & msg, std::string & err)> send;   // channel send; true on success
    // Optional: after a run that failed to reach the phone or to send, the next try waits 30 minutes. Points at a
    // value the caller keeps between ticks (0 = never tried).
    std::time_t * last_attempt = nullptr;
};
enum class WeeklyOutcome { NotDue, Sent, QuietWeek, NotEnoughData, PhoneUnreachable, SendFailed, NoChannel };
const char * outcome_name(WeeklyOutcome o);
// One scheduler tick: decides, pulls, sends at most one message and records the run date. Logs one line without
// numbers (to stderr) only when it did something. `now` is injected for tests.
WeeklyOutcome weekly_tick(const HealthConfig & cfg, bool toggle_on, std::time_t now, const WeeklyEnv & env);
std::string read_last_run(const std::string & data_dir);
bool write_last_run(const std::string & data_dir, const std::string & date);

// ---- tool ------------------------------------------------------------------------------------------------------
void register_health_tools(ToolRegistry & reg);
ToolResult run_query(const HealthConfig & cfg, const json & args, ToolContext & ctx, const std::string & today);

// ---- status (GET /api/health/status) ------------------------------------------------------------------------------
json status_json(const HealthConfig & cfg, bool weekly_on);

}  // namespace judas::health
