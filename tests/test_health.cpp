// Judas Health Link (desktop side): config validation, week_vs_normal math and thresholds, message wording, the
// Saturday guard, the weekly tick against a fake phone, the health_query tool (200, 401, 403, 503, timeout, down),
// and the health taint (every gated tool, the store flag, session_search, shell without network).
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "channels/channels.h"
#include "cpp-httplib/httplib.h"
#include "health/health.h"
#include "store/store.h"
#include "test.h"
#include "tools/session_tools.h"
#include "tools/tools.h"

using namespace judas;
using namespace judas::health;
namespace fs = std::filesystem;

static bool has(const std::string & s, const std::string & n) { return s.find(n) != std::string::npos; }
static bool no_em_dash(const std::string & s) { return !has(s, "\xE2\x80\x94") && !has(s, "\xE2\x80\x93"); }

static fs::path tmp_dir(const char * name) {
    fs::path d = fs::path(JUDAS_TEST_TMP) / "judas-test-health" / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

// ---- fake phone -------------------------------------------------------------------------------------------------
struct FakePhone {
    httplib::Server svr;
    std::thread th;
    int port = 0;
    std::atomic<int> force_status{0};      // 401 / 403 / 503 for every query
    std::atomic<int> delay_ms{0};
    std::atomic<int> queries{0};
    std::string token = "tok123";
    // per-metric rows: keyed by metric
    json sleep_rows = json::array(), rhr_rows = json::array(), hrv_rows = json::array();

    FakePhone() {
        svr.Get("/health/v1/ping", [this](const httplib::Request & req, httplib::Response & res) {
            if (req.get_header_value("Authorization") != "Bearer " + token) { res.status = 401; return; }
            res.set_content(R"({"ok":true,"app_version":"1.3.0","health_connect":"available","granted":["sleep","hrv"],"background_read":true,"tz":"America/Chicago"})", "application/json");
        });
        svr.Get("/health/v1/query", [this](const httplib::Request & req, httplib::Response & res) {
            queries++;
            if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            if (req.get_header_value("Authorization") != "Bearer " + token) { res.status = 401; res.set_content(R"({"error":"unauthorized"})", "application/json"); return; }
            std::string m = req.get_param_value("metric");
            if (force_status == 403) { res.status = 403; res.set_content(json{{"error", "permission"}, {"metric", m}}.dump(), "application/json"); return; }
            if (force_status == 503) { res.status = 503; res.set_content(R"({"error":"unavailable"})", "application/json"); return; }
            json rows = m == "sleep" ? sleep_rows : m == "resting_hr" ? rhr_rows : m == "hrv" ? hrv_rows : json::array();
            json out = {{"metric", m}, {"from", req.get_param_value("from")}, {"to", req.get_param_value("to")},
                        {"granularity", "day"}, {"unit", "x"}, {"source_apps", json::array()}, {"rows", rows}, {"truncated", false}};
            res.set_content(out.dump(), "application/json");
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    ~FakePhone() { svr.stop(); if (th.joinable()) th.join(); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

static const std::string TODAY = "2026-11-08";   // a Sunday

// Fill rows: this week (today-6..today) gets `wk` for the first wk_days days, the normal window gets `nv` for nd days.
static json make_rows(const char * key, double wk, int wk_days, double nv, int nd) {
    json rows = json::array();
    for (int i = 0; i < wk_days; i++) rows.push_back({{"date", add_days(TODAY, -i)}, {key, wk}});
    for (int i = 0; i < nd; i++) rows.push_back({{"date", add_days(TODAY, -7 - i)}, {key, nv}});
    return rows;
}

static std::tm tm_at(int y, int mo, int d, int h, int mi = 0) {
    std::tm t{};
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
    std::mktime(&t);   // fills tm_wday
    return t;
}
static std::time_t time_at(int y, int mo, int d, int h, int mi = 0) {
    std::tm t = tm_at(y, mo, d, h, mi);
    return std::mktime(&t);
}

// ---- tests ------------------------------------------------------------------------------------------------------
static void test_config() {
    fs::path d = tmp_dir("config");
    auto write = [&](const std::string & body) {
        std::ofstream(d / "c.toml") << body;
        return (d / "c.toml").string();
    };
    Config ok = load_config_file(write("[health]\nenabled = true\nurl = \"http://phone:8765\"\nweekly_day = \"sun\"\nweekly_hour = 19\ntimeout_s = 5\n"));
    CHECK(ok.health.enabled && ok.health.url == "http://phone:8765" && ok.health.weekly_hour == 19 && ok.health.timeout_s == 5);
    CHECK(ok.health.token_env == "JUDAS_HEALTH_TOKEN" && ok.health.weekly_checkin);

    bool threw = false;
    try { load_config_file(write("[health]\nweekly_day = \"sat\"\n")); }
    catch (const std::exception & e) { threw = true; CHECK(has(e.what(), "Saturday") && has(e.what(), "weekly_day")); }
    CHECK(threw);
    threw = false;
    try { load_config_file(write("[health]\nbogus = 1\n")); } catch (const std::exception &) { threw = true; }
    CHECK(threw);

    CHECK(health_weekly_day_error("sun").empty() && health_weekly_day_error("mon").empty());
    CHECK(!health_weekly_day_error("sat").empty());
    CHECK(!health_weekly_day_error("funday").empty());
    HealthConfig h;
    CHECK(health_config_problems(h).empty());
    h.weekly_hour = 24;
    CHECK(health_config_problems(h).size() == 1);
    h = HealthConfig{}; h.enabled = true; h.url = "phone:8765"; h.token_env = "";
    CHECK(health_config_problems(h).size() == 2);
    h = HealthConfig{}; h.weekly_day = "sat";
    CHECK(health_config_problems(h).size() == 1);
    Config c;
    c.health.weekly_day = "sat";
    bool saw = false;
    for (auto & p : validate_config(c)) if (has(p, "Saturday")) saw = true;
    CHECK(saw);
    CHECK(has(dump_config(ok), "[health]") && has(dump_config(ok), "weekly_day = ") && has(dump_config(ok), "sun"));
}

static void test_dates() {
    CHECK(add_days("2026-11-08", -6) == "2026-11-02");
    CHECK(add_days("2026-03-01", -1) == "2026-02-28");
    CHECK(add_days("2028-03-01", -1) == "2028-02-29");
    CHECK(add_days("2026-12-30", 3) == "2027-01-02");
    CHECK(weekday_of("2026-11-08") == 0 && weekday_of("2026-11-07") == 6 && weekday_of("2026-11-09") == 1);
    CHECK(valid_date("2026-02-28") && !valid_date("2026-02-29") && !valid_date("2026-13-01") && !valid_date("nope"));
    CHECK(weekday_from_name("sun") == 0 && weekday_from_name("sat") == 6 && weekday_from_name("x") == -1);
    CHECK(url_is_local("http://moto-g-5g---2024:8765") && url_is_local("http://100.101.2.3:8765") &&
          url_is_local("http://127.0.0.1:1") && url_is_local("https://x.example.ts.net/") && url_is_local("http://192.168.1.5"));
    CHECK(!url_is_local("https://api.example.com/v1") && !url_is_local("http://8.8.8.8") && !url_is_local("http://100.20.1.1"));
}

static void test_math_and_thresholds() {
    // sleep: normal 420 min, week 340 min (80 under): speaks. Resting HR 64 vs 58: speaks. HRV fine.
    Baseline b = compute_baseline(make_rows("asleep_min", 340, 7, 420, 28), make_rows("bpm", 64, 7, 58, 28),
                                  make_rows("rmssd_ms", 48, 7, 48, 28), TODAY);
    CHECK(b.week_from == "2026-11-02" && b.week_to == TODAY && b.normal_to == "2026-11-01" && b.normal_from == "2026-10-05");
    CHECK(b.sleep.week_n == 7 && b.sleep.normal_n == 28 && b.sleep.week_avg == 340 && b.sleep.normal_avg == 420);
    Triggers t = check_triggers(b);
    CHECK(t.sleep && t.rhr && !t.hrv && t.any());
    std::string msg = checkin_message(b);
    CHECK(msg == "Sleep averaged 5h40 this week, about 1h20 under your usual 7h00. Resting HR is 64, up 6 from your usual 58. Want to plan a lighter week?");
    CHECK(no_em_dash(msg));

    // exact boundaries: 60 min, 5 bpm, 15% all trigger; one under does not
    b = compute_baseline(make_rows("asleep_min", 360, 7, 420, 28), make_rows("bpm", 63, 7, 58, 28), make_rows("rmssd_ms", 42.5, 7, 50, 28), TODAY);
    t = check_triggers(b);
    CHECK(t.sleep && t.rhr && t.hrv);
    msg = checkin_message(b);
    CHECK(has(msg, "HRV averaged 43 ms, down 15% from your usual 50 ms."));   // 42.5 rounds to 43
    b = compute_baseline(make_rows("asleep_min", 361, 7, 420, 28), make_rows("bpm", 62.9, 7, 58, 28), make_rows("rmssd_ms", 43, 7, 50, 28), TODAY);
    CHECK(!check_triggers(b).any() && checkin_message(b).empty());
    // better than normal never speaks
    b = compute_baseline(make_rows("asleep_min", 480, 7, 420, 28), make_rows("bpm", 50, 7, 58, 28), make_rows("rmssd_ms", 70, 7, 50, 28), TODAY);
    CHECK(!check_triggers(b).any());

    // data minimums: 4 days this week and 14 in the normal window
    b = compute_baseline(make_rows("asleep_min", 300, 4, 420, 14), json::array(), json::array(), TODAY);
    CHECK(check_triggers(b).sleep);
    b = compute_baseline(make_rows("asleep_min", 300, 3, 420, 28), json::array(), json::array(), TODAY);
    CHECK(!check_triggers(b).sleep);
    b = compute_baseline(make_rows("asleep_min", 300, 7, 420, 13), json::array(), json::array(), TODAY);
    CHECK(!check_triggers(b).sleep);
    // per metric: sleep lacks data, resting HR has it
    b = compute_baseline(make_rows("asleep_min", 300, 2, 420, 28), make_rows("bpm", 70, 5, 58, 20), json::array(), TODAY);
    t = check_triggers(b);
    CHECK(!t.sleep && t.rhr);
    CHECK(!has(checkin_message(b), "Sleep"));
    // rows outside both windows (older than 34 days, or in the future) are ignored
    json far = json::array({{{"date", "2026-09-01"}, {"asleep_min", 100}}, {{"date", "2026-11-09"}, {"asleep_min", 100}}});
    b = compute_baseline(far, json::array(), json::array(), TODAY);
    CHECK(b.sleep.week_n == 0 && b.sleep.normal_n == 0);

    // tool JSON: numbers and deltas, no verdict words
    b = compute_baseline(make_rows("asleep_min", 340, 7, 420, 28), make_rows("bpm", 64, 7, 58, 28), make_rows("rmssd_ms", 40, 7, 50, 28), TODAY);
    json j = baseline_json(b);
    CHECK(j["sleep"]["delta"] == -80.0 && j["resting_hr"]["delta"] == 6.0 && j["hrv"]["delta_pct"] == -20.0);
    CHECK(j["sleep"]["days_this_week"] == 7 && j["sleep"]["days_in_normal"] == 28);
    std::string dumped = j.dump();
    for (const char * w : {"score", "grade", "streak", "goal", "good", "bad", "speak"}) CHECK(!has(dumped, w));
    j = baseline_json(compute_baseline(json::array(), json::array(), json::array(), TODAY));
    CHECK(j["sleep"]["this_week_avg"].is_null() && !j["sleep"].contains("delta"));
}

static void test_format() {
    CHECK(format_duration(340) == "5h40" && format_duration(420) == "7h00" && format_duration(425) == "7h05");
    CHECK(format_duration(80) == "1h20" && format_duration(45) == "45 min" && format_duration(0) == "0 min");
    // sleep only
    Baseline b = compute_baseline(make_rows("asleep_min", 330, 6, 400, 20), json::array(), json::array(), TODAY);
    CHECK(checkin_message(b) == "Sleep averaged 5h30 this week, about 1h10 under your usual 6h40. Want to plan a lighter week?");
}

static void test_quiet_and_due() {
    HealthConfig c;
    c.enabled = true;
    std::tm sun19 = tm_at(2026, 11, 8, 19), sun18 = tm_at(2026, 11, 8, 18), sat19 = tm_at(2026, 11, 7, 19), mon19 = tm_at(2026, 11, 9, 19);
    CHECK(sun19.tm_wday == 0 && sat19.tm_wday == 6);
    CHECK(weekly_due(c, true, sun19, "", false));
    CHECK(weekly_due(c, true, tm_at(2026, 11, 8, 22), "", false));        // daemon was down at 19:00, still the same day
    CHECK(!weekly_due(c, true, sun18, "", false));                        // too early
    CHECK(!weekly_due(c, true, mon19, "", false));                        // wrong day
    CHECK(!weekly_due(c, false, sun19, "", false));                       // toggled off
    CHECK(!weekly_due(c, true, sun19, "", true));                         // quiet hours
    CHECK(!weekly_due(c, true, sun19, "2026-11-08", false));              // already ran today (restart safe)
    CHECK(weekly_due(c, true, sun19, "2026-11-01", false));
    HealthConfig off = c; off.enabled = false;
    CHECK(!weekly_due(off, true, sun19, "", false));
    // Saturday guard, even if a config somehow said sat
    HealthConfig sat = c; sat.weekly_day = "sat";
    CHECK(!weekly_due(sat, true, sat19, "", false));
    CHECK(!weekly_due(c, true, sat19, "", false));
    // quiet hours spec
    std::tm t23 = tm_at(2026, 11, 8, 23, 30), t3 = tm_at(2026, 11, 8, 3), t12 = tm_at(2026, 11, 8, 12);
    CHECK(in_quiet("22:00-07:00", t23) && in_quiet("22:00-07:00", t3) && !in_quiet("22:00-07:00", t12));
    CHECK(in_quiet("09:00-17:00", t12) && !in_quiet("09:00-17:00", t23) && !in_quiet("", t12) && !in_quiet("junk", t12));
}

static void test_tool(FakePhone & ph) {
    setenv("JUDAS_HEALTH_TOKEN", "tok123", 1);
    Config cfg;
    cfg.health.enabled = true;
    cfg.health.url = ph.url();
    cfg.health.timeout_s = 1;
    ph.sleep_rows = json::array({{{"date", "2026-11-07"}, {"asleep_min", 400}}});
    int tainted = 0;
    ToolContext ctx;
    ctx.cfg = &cfg;
    ctx.taint_session = [&] { tainted++; };

    ToolRegistry reg;
    register_health_tools(reg);
    const Tool * tool = reg.find("health_query");
    REQUIRE(tool != nullptr);
    CHECK(!tool->writes && has(tool->description, "no scores") == false);
    CHECK(has(tool->description, "Never give scores") && no_em_dash(tool->description));

    auto r = reg.call("health_query", {{"metric", "sleep"}, {"from", "2026-11-01"}, {"to", "2026-11-08"}}, ctx);
    CHECK(r.ok && has(r.output, "\"asleep_min\":400") && tainted == 1);
    r = reg.call("health_query", {{"metric", "bogus"}}, ctx);
    CHECK(!r.ok);
    r = reg.call("health_query", {{"metric", "sleep"}, {"from", "2026-01-01"}, {"to", "2026-11-08"}}, ctx);
    CHECK(!r.ok && has(r.output, "120 days"));
    r = reg.call("health_query", {{"metric", "sleep"}, {"from", "2026-02-30"}}, ctx);
    CHECK(!r.ok && has(r.output, "dates"));
    CHECK(tainted == 1);   // failures never taint

    // week_vs_normal through the tool
    ph.sleep_rows = make_rows("asleep_min", 340, 7, 420, 28);
    ph.rhr_rows = make_rows("bpm", 64, 7, 58, 28);
    ph.hrv_rows = make_rows("rmssd_ms", 48, 7, 48, 28);
    json a = {{"metric", "week_vs_normal"}};
    auto q = run_query(cfg.health, a, ctx, TODAY);
    CHECK(q.ok);
    json j = json::parse(q.output);
    CHECK(j["sleep"]["delta"] == -80.0 && j["resting_hr"]["delta"] == 6.0 && j["hrv"]["delta"] == 0.0);
    CHECK(tainted == 2);

    // errors: 401, 403, 503, timeout, down, no token, disabled, remote active model
    ph.token = "other";
    q = run_query(cfg.health, {{"metric", "sleep"}}, ctx, TODAY);
    CHECK(!q.ok && has(q.output, "401") && has(q.output, "JUDAS_HEALTH_TOKEN"));
    ph.token = "tok123";
    ph.force_status = 403;
    q = run_query(cfg.health, {{"metric", "hrv"}}, ctx, TODAY);
    CHECK(!q.ok && has(q.output, "permission not granted for hrv"));
    q = run_query(cfg.health, a, ctx, TODAY);   // week_vs_normal with every permission missing: notes, not an error
    CHECK(q.ok && has(q.output, "unavailable") && has(q.output, "permission not granted"));
    ph.force_status = 503;
    q = run_query(cfg.health, {{"metric", "sleep"}}, ctx, TODAY);
    CHECK(!q.ok && has(q.output, "Health Connect is unavailable"));
    q = run_query(cfg.health, a, ctx, TODAY);
    CHECK(!q.ok && has(q.output, "Health Connect is unavailable"));
    ph.force_status = 0;
    ph.delay_ms = 2500;
    q = run_query(cfg.health, {{"metric", "sleep"}}, ctx, TODAY);
    CHECK(!q.ok && has(q.output, "phone not reachable") && has(q.output, "within 1 s"));
    ph.delay_ms = 0;
    HealthConfig dead = cfg.health;
    dead.url = "http://127.0.0.1:1";
    q = run_query(dead, {{"metric", "sleep"}}, ctx, TODAY);
    CHECK(!q.ok && has(q.output, "phone not reachable at"));
    unsetenv("JUDAS_HEALTH_TOKEN");
    q = run_query(cfg.health, {{"metric", "sleep"}}, ctx, TODAY);
    CHECK(!q.ok && has(q.output, "no token"));
    setenv("JUDAS_HEALTH_TOKEN", "tok123", 1);
    HealthConfig offc = cfg.health;
    offc.enabled = false;
    CHECK(!run_query(offc, {{"metric", "sleep"}}, ctx, TODAY).ok);
    int before = tainted;
    Config remote = cfg;
    ModelConfig m;
    m.url = "https://api.example.com/v1";
    remote.models["main"] = m;
    remote.engine.model = "main";
    ToolContext rctx = ctx;
    rctx.cfg = &remote;
    q = run_query(remote.health, {{"metric", "sleep"}}, rctx, TODAY);
    CHECK(!q.ok && has(q.output, "remotely") && tainted == before);

    // status
    json st = status_json(cfg.health, true);
    CHECK(st["configured"] == true && st["reachable"] == true && st["granted"].size() == 2 && st["weekly_checkin"] == true);
    st = status_json(dead, false);
    CHECK(st["reachable"] == false && st["weekly_checkin"] == false && st.contains("error"));
    HealthConfig nt = cfg.health;
    nt.token_env = "JUDAS_NO_SUCH_TOKEN";
    CHECK(status_json(nt, true)["configured"] == false);
    for (auto & s : {st.dump(), status_json(cfg.health, true).dump()}) CHECK(no_em_dash(s));
}

static void test_weekly_tick(FakePhone & ph) {
    setenv("JUDAS_HEALTH_TOKEN", "tok123", 1);
    fs::path d = tmp_dir("weekly");
    HealthConfig cfg;
    cfg.enabled = true;
    cfg.url = ph.url();
    cfg.timeout_s = 2;
    ph.force_status = 0;
    ph.delay_ms = 0;
    std::vector<std::string> sent;
    bool send_ok = true;
    WeeklyEnv env;
    env.data_dir = d.string();
    env.send = [&](const std::string & m, std::string & err) {
        if (!send_ok) { err = "boom"; return false; }
        sent.push_back(m);
        return true;
    };
    std::time_t sun19 = time_at(2026, 11, 8, 19), sat19 = time_at(2026, 11, 7, 19);

    // a bad week
    ph.sleep_rows = make_rows("asleep_min", 340, 7, 420, 28);
    ph.rhr_rows = make_rows("bpm", 64, 7, 58, 28);
    ph.hrv_rows = make_rows("rmssd_ms", 48, 7, 48, 28);

    // not due: wrong time, toggle off, Saturday, quiet hours; the phone is never asked
    int q0 = ph.queries;
    CHECK(weekly_tick(cfg, true, time_at(2026, 11, 8, 18), env) == WeeklyOutcome::NotDue);
    CHECK(weekly_tick(cfg, false, sun19, env) == WeeklyOutcome::NotDue);
    CHECK(weekly_tick(cfg, true, sat19, env) == WeeklyOutcome::NotDue);
    WeeklyEnv quiet = env; quiet.quiet_hours = "18:00-20:00";
    CHECK(weekly_tick(cfg, true, sun19, quiet) == WeeklyOutcome::NotDue);
    CHECK(ph.queries == q0 && sent.empty());

    // phone down: silent skip, not recorded, retried later (30 min spacing when last_attempt is kept)
    HealthConfig dead = cfg; dead.url = "http://127.0.0.1:1";
    std::time_t attempt = 0;
    WeeklyEnv spaced = env; spaced.last_attempt = &attempt;
    CHECK(weekly_tick(dead, true, sun19, spaced) == WeeklyOutcome::PhoneUnreachable);
    CHECK(sent.empty() && read_last_run(env.data_dir).empty());
    CHECK(weekly_tick(dead, true, sun19 + 600, spaced) == WeeklyOutcome::NotDue);          // too soon
    CHECK(weekly_tick(cfg, true, sun19 + 2000, spaced) == WeeklyOutcome::Sent);            // 33 min later, phone is back
    CHECK(sent.size() == 1);
    sent.clear();
    fs::remove(d / "health-state.json");
    attempt = 0;

    // send failure: not recorded, tries again
    send_ok = false;
    CHECK(weekly_tick(cfg, true, sun19, env) == WeeklyOutcome::SendFailed);
    CHECK(read_last_run(env.data_dir).empty());
    send_ok = true;

    // one message, recorded; a restart (fresh env, same data dir) does not send again
    CHECK(weekly_tick(cfg, true, sun19, env) == WeeklyOutcome::Sent);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0] == "Sleep averaged 5h40 this week, about 1h20 under your usual 7h00. Resting HR is 64, up 6 from your usual 58. Want to plan a lighter week?");
    CHECK(read_last_run(env.data_dir) == "2026-11-08");
    WeeklyEnv restarted = env;
    CHECK(weekly_tick(cfg, true, sun19 + 3600, restarted) == WeeklyOutcome::NotDue);
    CHECK(sent.size() == 1);

    // a normal week: nothing sent, still recorded (no repeats), and the next Sunday can speak again
    fs::remove(d / "health-state.json");
    ph.sleep_rows = make_rows("asleep_min", 415, 7, 420, 28);
    ph.rhr_rows = make_rows("bpm", 58, 7, 58, 28);
    ph.hrv_rows = make_rows("rmssd_ms", 48, 7, 48, 28);
    sent.clear();
    CHECK(weekly_tick(cfg, true, sun19, env) == WeeklyOutcome::QuietWeek);
    CHECK(sent.empty() && read_last_run(env.data_dir) == "2026-11-08");

    // not enough data
    fs::remove(d / "health-state.json");
    ph.sleep_rows = make_rows("asleep_min", 200, 3, 420, 28);
    ph.rhr_rows = json::array();
    ph.hrv_rows = json::array();
    CHECK(weekly_tick(cfg, true, sun19, env) == WeeklyOutcome::NotEnoughData);
    CHECK(sent.empty());

    // no send function configured
    fs::remove(d / "health-state.json");
    ph.sleep_rows = make_rows("asleep_min", 340, 7, 420, 28);
    WeeklyEnv nosend = env; nosend.send = nullptr;
    CHECK(weekly_tick(cfg, true, sun19, nosend) == WeeklyOutcome::NoChannel);
    CHECK(read_last_run(env.data_dir).empty());
    for (auto o : {WeeklyOutcome::NotDue, WeeklyOutcome::Sent, WeeklyOutcome::PhoneUnreachable}) CHECK(std::string(outcome_name(o)).size() > 0);
}

static void test_taint() {
    // The registry refuses every outbound tool in a tainted session and nothing else.
    CHECK(health_taint_refusal("memory_append", "").size() > 0);
    CHECK(health_taint_refusal("skill_manage", "").size() > 0);
    CHECK(health_taint_refusal("web_search", "").size() > 0);
    CHECK(health_taint_refusal("web_fetch", "").size() > 0);
    CHECK(health_taint_refusal("anything", "mcp:github").size() > 0);
    for (const char * ok : {"read_file", "write_file", "shell", "memory_read", "memory_search", "session_search", "health_query", "view_image", "done"})
        CHECK(health_taint_refusal(ok, "").empty());
    CHECK(has(health_taint_refusal("web_fetch", ""), "health data is in this chat") && no_em_dash(health_taint_refusal("web_fetch", "")));

    fs::path root = tmp_dir("taint");
    fs::create_directories(root / "work");
    Config cfg;
    cfg.permissions.allow_paths = {root.string()};
    cfg.permissions.mode = "full-auto";
    Journal journal(root / "journal");
    ToolRegistry reg;
    register_builtin_tools(reg);
    int ran = 0;
    for (const char * name : {"memory_append", "skill_manage"}) {   // stand-ins: the gate sits in the registry, before the tool runs
        Tool t;
        t.name = name;
        t.parameters = {{"type", "object"}, {"properties", json::object()}};
        t.run = [&](const json &, ToolContext &) { ran++; return ToolResult{true, "ran", false}; };
        reg.add(std::move(t));
    }
    Tool mcp;
    mcp.name = "gh__create_issue";
    mcp.source = "mcp:gh";
    mcp.parameters = {{"type", "object"}, {"properties", json::object()}};
    mcp.run = [&](const json &, ToolContext &) { ran++; return ToolResult{true, "ran", false}; };
    reg.add(std::move(mcp));

    ToolContext ctx;
    ctx.cwd = (root / "work").string();
    ctx.cfg = &cfg;
    ctx.journal = &journal;
    ctx.session = "s1";
    ctx.agent = "t";
    CHECK(reg.call("memory_append", json::object(), ctx).ok && reg.call("skill_manage", json::object(), ctx).ok &&
          reg.call("gh__create_issue", json::object(), ctx).ok && ran == 3);
    ran = 0;
    ctx.health_tainted = true;
    for (const char * name : {"memory_append", "skill_manage", "gh__create_issue"}) {
        auto r = reg.call(name, json::object(), ctx);
        CHECK(!r.ok && has(r.output, "health data is in this chat"));
    }
    CHECK(ran == 0);
    for (const char * name : {"web_search", "web_fetch"}) {
        auto r = reg.call(name, {{"query", "x"}, {"url", "http://127.0.0.1:1/"}}, ctx);
        CHECK(!r.ok && has(r.output, "health data is in this chat"));
    }
    // local tools still work
    {
        std::ofstream(root / "work" / "a.txt") << "hello";
        auto r = reg.call("read_file", {{"path", "a.txt"}}, ctx);
        CHECK(r.ok && has(r.output, "hello"));
    }
    // show_image refuses a URL but would still take a file
    register_attach_tools(reg);
    auto si = reg.call("show_image", {{"source", "http://127.0.0.1:1/x.png"}}, ctx);
    CHECK(!si.ok && has(si.output, "health data is in this chat"));
    // shell runs with the network cut off (bwrap), or refuses when bubblewrap is missing
    auto sh = reg.call("shell", {{"command", "echo hi"}}, ctx);
    if (find_bwrap().empty()) CHECK(!sh.ok && has(sh.output, "bubblewrap"));
    else CHECK(sh.ok ? has(sh.output, "hi") : true);
}

static void test_store() {
    fs::path d = tmp_dir("store");
    std::string db = (d / "s.db").string();
    std::string a, b;
    {
        Store st(db);
        a = st.create_session("tainted chat", "/tmp", "m").id;
        b = st.create_session("clean chat", "/tmp", "m").id;
        CHECK(!st.session_health_tainted(a) && !st.session_health_tainted(b));
        CHECK(st.set_session_health_tainted(a));
        CHECK(!st.set_session_health_tainted("nope"));
        st.add_message(a, "ag", "tool", json{{"role", "tool"}, {"content", "asleep_min 340 zebra"}});
        st.add_message(b, "ag", "user", json{{"role", "user"}, {"content", "zebra crossing"}});
    }
    {
        Store st(db);   // reopened: the flag persisted
        CHECK(st.session_health_tainted(a) && !st.session_health_tainted(b));
        CHECK(st.search_messages("zebra", 10, "", false, false).size() == 2);
        auto only = st.search_messages("zebra", 10, "", false, true);
        CHECK(only.size() == 1 && only[0].session == b);

        // session_search from a clean session never surfaces the tainted one; from a tainted session it can
        ToolRegistry reg;
        register_session_tools(reg, st);
        ToolContext ctx;
        auto r = reg.call("session_search", {{"query", "zebra"}}, ctx);
        CHECK(r.ok && !has(r.output, a) && has(r.output, b));
        ctx.health_tainted = true;
        r = reg.call("session_search", {{"query", "zebra"}}, ctx);
        CHECK(r.ok && has(r.output, a));
    }
}

int main() {
    test_config();
    test_dates();
    test_math_and_thresholds();
    test_format();
    test_quiet_and_due();
    {
        FakePhone ph;
        test_tool(ph);
        test_weekly_tick(ph);
    }
    test_taint();
    test_store();
    TEST_MAIN_END();
}
