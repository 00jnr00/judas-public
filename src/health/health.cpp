// Judas Health Link, desktop side. See health.h.
#include "health/health.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "cpp-httplib/httplib.h"

namespace judas::health {
namespace fs = std::filesystem;
namespace {

std::string base_of(const std::string & url) {
    std::string u = url;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

std::string host_of(const std::string & url) {
    size_t p = url.find("://");
    std::string rest = p == std::string::npos ? url : url.substr(p + 3);
    size_t end = rest.find_first_of("/?#");
    if (end != std::string::npos) rest = rest.substr(0, end);
    size_t at = rest.rfind('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    if (!rest.empty() && rest[0] == '[') {   // [::1]:port
        size_t rb = rest.find(']');
        return rb == std::string::npos ? rest : rest.substr(1, rb - 1);
    }
    size_t colon = rest.find(':');
    if (colon != std::string::npos) rest = rest.substr(0, colon);
    for (auto & c : rest) c = (char) std::tolower((unsigned char) c);
    return rest;
}

long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long) doe - 719468;
}

void civil_from_days(long long z, int & y, unsigned & m, unsigned & d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned) (z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int) (yoe + era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}

bool parse_date(const std::string & s, int & y, unsigned & m, unsigned & d) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i : {0, 1, 2, 3, 5, 6, 8, 9})
        if (!std::isdigit((unsigned char) s[i])) return false;
    y = std::atoi(s.substr(0, 4).c_str());
    m = (unsigned) std::atoi(s.substr(5, 2).c_str());
    d = (unsigned) std::atoi(s.substr(8, 2).c_str());
    if (m < 1 || m > 12 || d < 1) return false;
    static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int lim = dim[m - 1];
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (m == 2 && leap) lim = 29;
    return (int) d <= lim;
}

double round1(double v) { return std::round(v * 10.0) / 10.0; }

// Average of `key` over rows whose date is in [from, to] (string compare works for ISO dates).
Stat window(const json & rows, const char * key, const std::string & from, const std::string & to) {
    Stat s;
    double sum = 0;
    int n = 0;
    if (!rows.is_array()) return s;
    for (const auto & r : rows) {
        if (!r.is_object() || !r.contains("date") || !r["date"].is_string()) continue;
        std::string d = r["date"].get<std::string>();
        if (d < from || d > to) continue;
        if (!r.contains(key) || !r[key].is_number()) continue;
        sum += r[key].get<double>();
        n++;
    }
    s.week_n = n;
    s.week_avg = n ? sum / n : 0;
    return s;
}

void fill(Stat & out, const json & rows, const char * key, const Baseline & b) {
    Stat w = window(rows, key, b.week_from, b.week_to);
    Stat n = window(rows, key, b.normal_from, b.normal_to);
    out.week_n = w.week_n;
    out.week_avg = w.week_avg;
    out.normal_n = n.week_n;
    out.normal_avg = n.week_avg;
}

bool enough(const Stat & s) { return s.week_n >= 4 && s.normal_n >= 14; }

}  // namespace

// ---- phone client ------------------------------------------------------------------------------------------------
bool url_is_local(const std::string & url) {
    std::string h = host_of(url);
    if (h.empty()) return false;
    if (h == "localhost" || h == "::1" || h.rfind("127.", 0) == 0 || h.rfind("10.", 0) == 0 || h.rfind("192.168.", 0) == 0) return true;
    if (h.size() > 7 && h.compare(h.size() - 7, 7, ".ts.net") == 0) return true;
    if (h.rfind("172.", 0) == 0) {
        int b = std::atoi(h.c_str() + 4);
        return b >= 16 && b <= 31;
    }
    if (h.rfind("100.", 0) == 0) {
        int b = std::atoi(h.c_str() + 4);
        return b >= 64 && b <= 127;
    }
    return h.find('.') == std::string::npos && h.find(':') == std::string::npos;   // a bare tailnet / LAN name
}

Reply phone_get(const HealthConfig & cfg, const std::string & path_query) {
    Reply r;
    const char * tok = cfg.token_env.empty() ? nullptr : std::getenv(cfg.token_env.c_str());
    if (!tok || !*tok) { r.transport = "no token"; return r; }
    std::string base = base_of(cfg.url);
    if (base.rfind("http://", 0) != 0 && base.rfind("https://", 0) != 0) { r.transport = "bad url"; return r; }
    httplib::Client cli(base);
    int t = std::clamp(cfg.timeout_s, 1, 120);
    cli.set_connection_timeout(t);
    cli.set_read_timeout(t);
    cli.set_write_timeout(t);
    cli.set_follow_location(false);
    httplib::Headers h = {{"Authorization", std::string("Bearer ") + tok}, {"Accept", "application/json"}};
    auto res = cli.Get(path_query, h);
    if (!res) {
        auto e = res.error();
        r.transport = (e == httplib::Error::ConnectionTimeout || e == httplib::Error::Read || e == httplib::Error::Write) ? "timeout" : "unreachable";
        return r;
    }
    r.status = res->status;
    r.body = json::parse(res->body, nullptr, false);
    return r;
}

std::string failure_text(const Reply & r, const std::string & metric, const HealthConfig & cfg) {
    if (r.transport == "no token")
        return "health link has no token: set " + cfg.token_env + " in the daemon's environment (secrets.env)";
    if (r.transport == "bad url") return "health.url is not an http(s) URL";
    if (r.transport == "timeout") return "phone not reachable: no answer from " + cfg.url + " within " + std::to_string(cfg.timeout_s) + " s";
    if (!r.transport.empty())
        return "phone not reachable at " + cfg.url + " (is the Health link on in the Judas app, and the phone on the tailnet?)";
    if (r.status == 200) return "";
    if (r.status == 401) return "phone rejected the token (401): " + cfg.token_env + " does not match the token in the app's Health settings";
    if (r.status == 403) return "permission not granted for " + (metric.empty() ? std::string("this metric") : metric) + " (grant it in the Judas app's Health settings)";
    if (r.status == 503) return "Health Connect is unavailable on the phone (not installed or needs an update)";
    if (r.status == 400) {
        std::string why = r.body.is_object() && r.body.contains("error") && r.body["error"].is_string() ? r.body["error"].get<std::string>() : "bad request";
        return "phone rejected the request: " + why;
    }
    return "phone returned HTTP " + std::to_string(r.status);
}

// ---- dates -------------------------------------------------------------------------------------------------------
std::string date_of(std::time_t t) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

bool valid_date(const std::string & d) {
    int y; unsigned m, dd;
    return parse_date(d, y, m, dd);
}

std::string add_days(const std::string & date, int n) {
    int y; unsigned m, d;
    if (!parse_date(date, y, m, d)) return date;
    int y2; unsigned m2, d2;
    civil_from_days(days_from_civil(y, m, d) + n, y2, m2, d2);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y2, m2, d2);
    return buf;
}

int weekday_of(const std::string & date) {
    int y; unsigned m, d;
    if (!parse_date(date, y, m, d)) return -1;
    long long z = days_from_civil(y, m, d);   // 1970-01-01 (day 0) was a Thursday
    return (int) (((z % 7) + 7 + 4) % 7);
}

int weekday_from_name(const std::string & n) {
    static const char * names[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
    for (int i = 0; i < 7; i++) if (n == names[i]) return i;
    return -1;
}

// ---- week vs normal ----------------------------------------------------------------------------------------------
Baseline compute_baseline(const json & sleep_rows, const json & rhr_rows, const json & hrv_rows, const std::string & today) {
    Baseline b;
    b.week_to = today;
    b.week_from = add_days(today, -6);
    b.normal_to = add_days(today, -7);
    b.normal_from = add_days(today, -34);
    fill(b.sleep, sleep_rows, "asleep_min", b);
    fill(b.rhr, rhr_rows, "bpm", b);
    fill(b.hrv, hrv_rows, "rmssd_ms", b);
    return b;
}

std::optional<Baseline> fetch_baseline(const HealthConfig & cfg, const std::string & today, std::string & err,
                                       std::vector<std::string> * notes) {
    std::string from = add_days(today, -34);
    json rows[3];
    const char * metrics[3] = {"sleep", "resting_hr", "hrv"};
    for (int i = 0; i < 3; i++) {
        Reply r = phone_get(cfg, std::string("/health/v1/query?metric=") + metrics[i] + "&from=" + from + "&to=" + today + "&granularity=day");
        if (r.status == 403) {
            if (notes) notes->push_back(failure_text(r, metrics[i], cfg));
            continue;
        }
        if (std::string f = failure_text(r, metrics[i], cfg); !f.empty()) { err = f; return std::nullopt; }
        if (r.body.is_object() && r.body.contains("rows")) rows[i] = r.body["rows"];
    }
    return compute_baseline(rows[0], rows[1], rows[2], today);
}

json baseline_json(const Baseline & b) {
    auto one = [](const Stat & s, bool pct) {
        json j = {{"days_this_week", s.week_n}, {"days_in_normal", s.normal_n}};
        j["this_week_avg"] = s.week_n ? json(round1(s.week_avg)) : json(nullptr);
        j["normal_avg"] = s.normal_n ? json(round1(s.normal_avg)) : json(nullptr);
        if (s.week_n && s.normal_n) {
            j["delta"] = round1(s.week_avg - s.normal_avg);
            if (pct && s.normal_avg > 0) j["delta_pct"] = round1((s.week_avg - s.normal_avg) / s.normal_avg * 100.0);
        }
        return j;
    };
    json sleep = one(b.sleep, false), rhr = one(b.rhr, false), hrv = one(b.hrv, true);
    sleep["unit"] = "minutes asleep per night";
    rhr["unit"] = "bpm";
    hrv["unit"] = "rmssd ms";
    return {{"this_week", {{"from", b.week_from}, {"to", b.week_to}}},
            {"normal", {{"from", b.normal_from}, {"to", b.normal_to}, {"note", "the 28 days before this week"}}},
            {"sleep", sleep}, {"resting_hr", rhr}, {"hrv", hrv}};
}

Triggers check_triggers(const Baseline & b) {
    Triggers t;
    t.sleep = enough(b.sleep) && b.sleep.normal_avg - b.sleep.week_avg >= 60.0;
    t.rhr = enough(b.rhr) && b.rhr.week_avg - b.rhr.normal_avg >= 5.0;
    t.hrv = enough(b.hrv) && b.hrv.normal_avg > 0 && (b.hrv.normal_avg - b.hrv.week_avg) / b.hrv.normal_avg >= 0.15;
    return t;
}

std::string format_duration(int minutes) {
    if (minutes < 0) minutes = 0;
    if (minutes < 60) return std::to_string(minutes) + " min";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%dh%02d", minutes / 60, minutes % 60);
    return buf;
}

std::string checkin_message(const Baseline & b) {
    Triggers t = check_triggers(b);
    if (!t.any()) return "";
    std::string out;
    auto add = [&](const std::string & s) { out += (out.empty() ? "" : " ") + s; };
    if (t.sleep) {
        int w = (int) std::lround(b.sleep.week_avg), n = (int) std::lround(b.sleep.normal_avg);
        add("Sleep averaged " + format_duration(w) + " this week, about " + format_duration(n - w) + " under your usual " +
            format_duration(n) + ".");
    }
    if (t.rhr) {
        int w = (int) std::lround(b.rhr.week_avg), n = (int) std::lround(b.rhr.normal_avg);
        add("Resting HR is " + std::to_string(w) + ", up " + std::to_string(w - n) + " from your usual " + std::to_string(n) + ".");
    }
    if (t.hrv) {
        int w = (int) std::lround(b.hrv.week_avg), n = (int) std::lround(b.hrv.normal_avg);
        int pct = (int) std::lround((b.hrv.normal_avg - b.hrv.week_avg) / b.hrv.normal_avg * 100.0);
        add("HRV averaged " + std::to_string(w) + " ms, down " + std::to_string(pct) + "% from your usual " + std::to_string(n) + " ms.");
    }
    add("Want to plan a lighter week?");
    return out;
}

// ---- weekly check-in ---------------------------------------------------------------------------------------------
bool in_quiet(const std::string & spec, const std::tm & now) {
    auto dash = spec.find('-');
    if (dash == std::string::npos) return false;
    auto mins = [](const std::string & s) -> int {
        auto c = s.find(':');
        if (c == std::string::npos) return -1;
        try { return std::stoi(s.substr(0, c)) * 60 + std::stoi(s.substr(c + 1)); } catch (...) { return -1; }
    };
    int a = mins(spec.substr(0, dash)), b = mins(spec.substr(dash + 1));
    if (a < 0 || b < 0) return false;
    int n = now.tm_hour * 60 + now.tm_min;
    return a <= b ? (n >= a && n < b) : (n >= a || n < b);
}

bool weekly_due(const HealthConfig & cfg, bool toggle_on, const std::tm & now, const std::string & last_run, bool quiet) {
    if (!cfg.enabled || !toggle_on || quiet) return false;
    if (now.tm_wday == 6) return false;                           // Saturday is rest day, whatever the config says
    int want = weekday_from_name(cfg.weekly_day);
    if (want < 0 || want == 6 || now.tm_wday != want) return false;
    if (now.tm_hour < cfg.weekly_hour) return false;
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &now);
    return last_run != buf;                                        // one run per day, and the day is the weekly day
}

const char * outcome_name(WeeklyOutcome o) {
    switch (o) {
        case WeeklyOutcome::NotDue: return "not due";
        case WeeklyOutcome::Sent: return "sent";
        case WeeklyOutcome::QuietWeek: return "nothing to report";
        case WeeklyOutcome::NotEnoughData: return "not enough data";
        case WeeklyOutcome::PhoneUnreachable: return "phone not available";
        case WeeklyOutcome::SendFailed: return "send failed";
        case WeeklyOutcome::NoChannel: return "no channel";
    }
    return "";
}

static std::string state_path(const std::string & data_dir) { return (fs::path(data_dir) / "health-state.json").string(); }

std::string read_last_run(const std::string & data_dir) {
    std::ifstream in(state_path(data_dir));
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    return j.is_object() && j.contains("last_run") && j["last_run"].is_string() ? j["last_run"].get<std::string>() : "";
}

bool write_last_run(const std::string & data_dir, const std::string & date) {
    std::error_code ec;
    fs::create_directories(data_dir, ec);
    std::string path = state_path(data_dir), tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << json{{"last_run", date}}.dump() << "\n";
        if (!out) return false;
    }
    fs::rename(tmp, path, ec);
    return !ec;
}

WeeklyOutcome weekly_tick(const HealthConfig & cfg, bool toggle_on, std::time_t now, const WeeklyEnv & env) {
    std::tm tm{};
    localtime_r(&now, &tm);
    std::string today = date_of(now);
    if (!weekly_due(cfg, toggle_on, tm, read_last_run(env.data_dir), in_quiet(env.quiet_hours, tm))) return WeeklyOutcome::NotDue;
    if (env.last_attempt && *env.last_attempt != 0 && now - *env.last_attempt < 1800) return WeeklyOutcome::NotDue;
    if (env.last_attempt) *env.last_attempt = now;
    auto log = [](const char * what) { std::fprintf(stderr, "judas health: weekly check-in: %s\n", what); };

    std::string err;
    auto base = fetch_baseline(cfg, today, err);
    if (!base) {
        log(("skipped, " + err).c_str());
        return WeeklyOutcome::PhoneUnreachable;
    }
    Triggers t = check_triggers(*base);
    if (!t.any()) {
        write_last_run(env.data_dir, today);
        bool any_data = enough(base->sleep) || enough(base->rhr) || enough(base->hrv);
        log(any_data ? "nothing to report" : "not enough data");
        return any_data ? WeeklyOutcome::QuietWeek : WeeklyOutcome::NotEnoughData;
    }
    if (!env.send) { log("no channel to send on"); return WeeklyOutcome::NoChannel; }
    std::string serr;
    if (!env.send(checkin_message(*base), serr)) {
        log(("send failed, " + serr).c_str());
        return WeeklyOutcome::SendFailed;
    }
    write_last_run(env.data_dir, today);
    log("sent");
    return WeeklyOutcome::Sent;
}

// ---- tool --------------------------------------------------------------------------------------------------------
ToolResult run_query(const HealthConfig & cfg, const json & a, ToolContext & ctx, const std::string & today) {
    auto fail = [](const std::string & m) { return ToolResult{false, m, false}; };
    if (!cfg.enabled) return fail("health_query: the health link is off ([health] enabled = false)");
    if (ctx.cfg) {   // never hand health data to a model that runs on someone else's server
        try {
            const auto & m = ctx.cfg->active_model();
            if (!m.url.empty() && !url_is_local(m.url))
                return fail("health_query refused: the active model runs remotely (" + m.url + "); health data stays on local models");
        } catch (...) {}
    }
    std::string metric = a.value("metric", "");
    static const char * allowed[] = {"sleep", "heart_rate", "resting_hr", "hrv", "steps", "week_vs_normal"};
    if (std::none_of(std::begin(allowed), std::end(allowed), [&](const char * m) { return metric == m; }))
        return fail("health_query: metric must be sleep, heart_rate, resting_hr, hrv, steps or week_vs_normal");

    if (metric == "week_vs_normal") {
        std::string err;
        std::vector<std::string> notes;
        auto base = fetch_baseline(cfg, today, err, &notes);
        if (!base) return fail("health_query: " + err);
        json j = baseline_json(*base);
        if (!notes.empty()) j["unavailable"] = notes;
        if (ctx.taint_session) ctx.taint_session();
        return {true, j.dump(), false};
    }

    std::string to = a.contains("to") && a["to"].is_string() ? a["to"].get<std::string>() : today;
    std::string from = a.contains("from") && a["from"].is_string() ? a["from"].get<std::string>() : add_days(to, -6);
    if (!valid_date(from) || !valid_date(to)) return fail("health_query: from and to must be dates like 2026-11-03");
    if (from > to) return fail("health_query: from is after to");
    if (add_days(from, 120) < to) return fail("health_query: the span is at most 120 days");
    std::string gran = a.value("granularity", "day");
    if (gran != "day" && gran != "raw") return fail("health_query: granularity must be day or raw");

    Reply r = phone_get(cfg, "/health/v1/query?metric=" + metric + "&from=" + from + "&to=" + to + "&granularity=" + gran);
    if (std::string f = failure_text(r, metric, cfg); !f.empty()) return fail("health_query: " + f);
    if (ctx.taint_session) ctx.taint_session();
    return {true, truncate_output(r.body.is_null() ? std::string("(empty reply)") : r.body.dump(), 30000), false};
}

void register_health_tools(ToolRegistry & reg) {
    Tool t;
    t.name = "health_query";
    t.description =
        "Read the user's own health data from his phone (Amazfit via Health Connect), read only. metric: sleep, "
        "heart_rate, resting_hr, hrv, steps, or week_vs_normal (this week against the 28 days before it, computed "
        "for you). from/to are local dates YYYY-MM-DD (default: the last 7 days), at most 120 days; granularity day "
        "or raw. Answer with plain facts and comparisons to his own history. Never give scores, grades, streaks, "
        "goals or medical diagnosis. After this call the chat is health-tainted: web, memory, skill and MCP tools "
        "are off for the rest of it.";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"metric", {{"type", "string"}, {"enum", {"sleep", "heart_rate", "resting_hr", "hrv", "steps", "week_vs_normal"}}}},
                      {"from", {{"type", "string"}, {"description", "First local date, YYYY-MM-DD (inclusive)"}}},
                      {"to", {{"type", "string"}, {"description", "Last local date, YYYY-MM-DD (inclusive); default today"}}},
                      {"granularity", {{"type", "string"}, {"enum", {"day", "raw"}}, {"description", "day (default) or raw samples"}}}}},
                    {"required", {"metric"}}};
    t.run = [](const json & a, ToolContext & ctx) -> ToolResult {
        if (!ctx.cfg) return {false, "health_query: no configuration available", false};
        return run_query(ctx.cfg->health, a, ctx, date_of(std::time(nullptr)));
    };
    reg.add(std::move(t));
}

json status_json(const HealthConfig & cfg, bool weekly_on) {
    const char * tok = cfg.token_env.empty() ? nullptr : std::getenv(cfg.token_env.c_str());
    bool token = tok && *tok;
    json j = {{"enabled", cfg.enabled}, {"configured", cfg.enabled && token && !cfg.url.empty()}, {"token_set", token},
              {"url", cfg.url}, {"reachable", false}, {"granted", json::array()}, {"weekly_checkin", weekly_on},
              {"weekly_day", cfg.weekly_day}, {"weekly_hour", cfg.weekly_hour}};
    if (!j["configured"].get<bool>()) return j;
    Reply r = phone_get(cfg, "/health/v1/ping");
    if (r.status == 200 && r.body.is_object()) {
        j["reachable"] = true;
        if (r.body.contains("granted")) j["granted"] = r.body["granted"];
        for (const char * k : {"health_connect", "background_read", "app_version"})
            if (r.body.contains(k)) j[k] = r.body[k];
    } else {
        j["error"] = failure_text(r, "", cfg);
    }
    return j;
}

}  // namespace judas::health
