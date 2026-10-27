// HTTP server: OpenAI-compatible /v1, Judas /api, /health, /state.
// One httplib::Server per bind address, each on its own thread. Handlers block (engine batching, SSE), so
// the task queue is a big thread pool. Request log lines carry method, path, status and ms only.
#include "server/server.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <condition_variable>
#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "cpp-httplib/httplib.h"
#include "agent/autoresume.h"
#include "channels/channels.h"
#include "core/json.h"
#include "core/skin.h"
#include "engine/engine.h"
#include "heartbeat/heartbeat.h"
#include "learning/curator.h"
#include "learning/learn.h"
#include "models/models.h"
#include "server/assets.h"
#include "server/event_hub.h"
#include "skills/skills.h"
#include "skills/usage.h"
#include "tools/memory_tools.h"
#include <linux/close_range.h>
#include <unistd.h>

namespace judas {

const char * version();

namespace {

using Clock = std::chrono::steady_clock;
thread_local Clock::time_point t_start;

std::string rand_hex(int n) {
    static std::mutex m;
    static std::mt19937_64 rng{std::random_device{}()};
    std::lock_guard lk(m);
    static const char * d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < n; i++) s += d[rng() & 15];
    return s;
}

json error_json(const std::string & msg, const std::string & type) {
    return {{"error", {{"message", msg}, {"type", type}}}};
}

void send_json(httplib::Response & res, const json & j, int status = 200) {
    res.status = status;
    res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

json session_json(const SessionRow & s) {
    return {{"id", s.id}, {"title", s.title}, {"cwd", s.cwd}, {"model", s.model}, {"mode", s.mode},
            {"auto_resume", s.auto_resume < 0 ? json(nullptr) : json(s.auto_resume > 0)},
            {"created_ms", s.created_ms}, {"updated_ms", s.updated_ms}};
}

bool valid_mode(const std::string & m) { return m == "chat" || m == "cowork" || m == "code"; }

// Max worker threads per listener (JUDAS_HTTP_THREADS, default 512, floor 32). Every open SSE stream holds
// one thread for as long as the page is open (two per browser tab: its session stream and the global feed),
// so this is a ceiling on concurrent streams plus in-flight requests. Idle extra threads exit after 3 s.
size_t http_max_threads() {
    long n = 512;
    if (const char * e = std::getenv("JUDAS_HTTP_THREADS")) n = std::atol(e);
    return (size_t) std::clamp(n, 32L, 16384L);
}

std::string dump_json(const json & j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }


json agent_json(const AgentRow & a) {
    return {{"id", a.id}, {"session", a.session}, {"parent", a.parent}, {"role", a.role}, {"task", a.task},
            {"status", a.status}, {"result", a.result}, {"created_ms", a.created_ms},
            {"updated_ms", a.updated_ms}, {"prompt_tokens", a.prompt_tokens},
            {"completion_tokens", a.completion_tokens}};
}

json checkpoint_json(const CheckpointInfo & c) {
    json files = json::array();
    for (auto & f : c.files)
        files.push_back({{"path", f.path}, {"before_hash", f.before_hash}, {"after_hash", f.after_hash},
                         {"before_is_dir", f.before_is_dir}});
    return {{"id", c.id}, {"session", c.session}, {"agent", c.agent}, {"label", c.label},
            {"created_ms", c.created_ms}, {"committed", c.committed}, {"reverted", c.reverted},
            {"files", files}, {"irreversible", c.irreversible}};
}

json revert_json(const RevertResult & r) {
    return {{"ok", r.ok}, {"restored", r.restored}, {"removed", r.removed}, {"conflicts", r.conflicts},
            {"irreversible", r.irreversible}, {"error", r.error}};
}

json state_json(const EngineState & s) {
    json seqs = json::array();
    for (auto & q : s.seqs)
        seqs.push_back({{"id", q.id}, {"tag", q.tag}, {"phase", q.phase}, {"tokens", q.tokens},
                        {"priority", q.priority}});
    json queue = json::array();
    for (auto & q : s.queue)
        queue.push_back({{"tag", q.tag}, {"priority", q.priority}, {"prompt_tokens", q.prompt_tokens},
                         {"waited_s", q.waited_s}, {"preempted", q.preempted}});
    return {{"ctx_active", s.ctx_active}, {"ctx_free", s.ctx_free}, {"kv_reserve", s.kv_reserve},
            {"preemptions", s.preemptions}, {"queue", queue},
            {"prefill_duty", s.prefill_duty}, {"prefill_chunk_busy", s.prefill_chunk_busy},
            {"prefill_max_wait_ms", s.prefill_max_wait_ms},
            {"model", s.model}, {"model_path", s.model_path}, {"loaded", s.loaded}, {"hybrid", s.hybrid},
            {"max_agents", s.max_agents}, {"active", s.active}, {"queued", s.queued},
            {"ctx_total", s.ctx_total}, {"ctx_used", s.ctx_used}, {"vram_used_mib", s.vram_used_mib},
            {"vram_total_mib", s.vram_total_mib}, {"gen_tok_s", s.gen_tok_s}, {"seqs", seqs},
            {"total_prompt_tokens", s.total_prompt_tokens},
            {"total_completion_tokens", s.total_completion_tokens}, {"requests", s.requests}};
}

json event_json(const AgentEvent & e) {
    return {{"type", e.type}, {"session", e.session}, {"agent", e.agent}, {"parent", e.parent},
            {"role", e.role}, {"data", e.data}};
}

std::string sse(const json & j) {
    return "data: " + j.dump(-1, ' ', false, json::error_handler_t::replace) + "\n\n";
}

// Pulls sampling and thinking settings out of an OpenAI request body.
ChatRequest parse_chat(const json & b, const std::string & user_agent) {
    ChatRequest r;
    r.messages = messages_from_oai(b.at("messages"));
    if (b.contains("tools") && b["tools"].is_array()) r.tools = tools_from_oai(b["tools"]);
    auto num = [&](const char * k, auto & dst) {
        if (b.contains(k) && b[k].is_number()) dst = b[k].get<std::decay_t<decltype(dst)>>();
    };
    num("temperature", r.sampling.temp);
    num("top_p", r.sampling.top_p);
    num("min_p", r.sampling.min_p);
    num("top_k", r.sampling.top_k);
    num("seed", r.sampling.seed);
    if (b.contains("max_tokens") && b["max_tokens"].is_number()) r.sampling.max_tokens = b["max_tokens"];
    if (b.contains("max_completion_tokens") && b["max_completion_tokens"].is_number())
        r.sampling.max_tokens = b["max_completion_tokens"];
    r.enable_thinking = true;
    if (b.contains("chat_template_kwargs") && b["chat_template_kwargs"].is_object()) {
        auto & k = b["chat_template_kwargs"];
        if (k.contains("enable_thinking") && k["enable_thinking"].is_boolean())
            r.enable_thinking = k["enable_thinking"];
    }
    if (b.contains("reasoning_effort") && b["reasoning_effort"].is_string()) {
        std::string e = b["reasoning_effort"];
        if (e == "none" || e == "off") r.enable_thinking = false;
    }
    // Same priority as a lead agent: at 1, a /v1 call lost every admission, prefill and preemption contest to
    // running agents and only produced tokens once they stopped (the "one burst at the end" stream).
    r.priority = 0;
    std::string ua = user_agent.substr(0, user_agent.find_first_of(" /"));
    r.tag = "v1 " + (ua.empty() ? std::string("client") : ua);
    return r;
}

json usage_json(const ChatResult & r) {
    return {{"prompt_tokens", r.prompt_tokens}, {"completion_tokens", r.completion_tokens},
            {"total_tokens", r.prompt_tokens + r.completion_tokens},
            {"prompt_tokens_details", {{"cached_tokens", r.cached_tokens}}}};
}

std::string finish_of(const ChatResult & r) {
    if (r.finish_reason == "length") return "length";   // a cut-off tool call is not a complete one
    if (!r.message.tool_calls.empty()) return "tool_calls";
    if (r.finish_reason == "cancelled" || r.finish_reason.empty()) return "stop";
    return r.finish_reason;
}

}  // namespace

struct Server::Impl {
    const Config & cfg;
    Engine & engine;
    AgentManager & agents;
    Store & store;
    Journal & journal;
    std::vector<std::unique_ptr<httplib::Server>> servers;
    std::atomic<bool> stopping{false};
    std::thread scheduler;
    std::thread heartbeat_thread;
    std::thread resume_thread;
    // Auto-resume bookkeeping per session (in memory; a restart forgets it, the per-day count included).
    struct ResumeTrack {
        std::string day;
        int attempts = 0, stalled = 0;
        bool pending = false;          // an auto-resume ran and its outcome is not judged yet
        long long writes_at = 0;       // file-changing tool calls seen when it was resumed
        int open_at = 0;               // open plan items when it was resumed
        long long resumed_ms = 0;
    };
    std::mutex resume_mu;
    std::map<std::string, ResumeTrack> resume_track;
    std::map<std::string, long long> paused_since;   // session -> when its lead turn paused (ms)
    std::map<std::string, long long> write_count;    // session -> file-changing tool calls so far
    // Last time an agent event arrived, for the curator's idle gate (0 = nothing observed since start).
    std::atomic<long long> last_agent_active_ms{0};
    long long curator_fail_ms = 0;  // last time the curator pass aborted; paces retries away from per-tick
    EventHub hub;                       // replayable event rings: one per session, "*" = global feed
    int listener_handle = 0;
    std::mutex floor_mu;
    std::map<std::string, long long> msg_floor;   // session -> last stored message id when its turn began
    std::mutex hb_mu;
    HeartbeatReport last_hb;

    // Summarize the stack facts with a model: the local engine, or a remote OpenAI-compatible endpoint when
    // heartbeat.endpoint is a URL (so it can run off the laptop when the GPU is busy). Returns (summary, severity).
    std::pair<std::string, std::string> summarize(const std::string & facts_text) {
        std::string sys =
            "You are a server monitor for one person's home machine. Given these read-only facts, reply with a "
            "first line exactly 'SEVERITY: ok' or notice/warn/critical, then 2 to 4 short sentences on what looks "
            "fine and anything that needs attention. Be concrete and calm.";
        std::string reply;
        const std::string & ep = cfg.heartbeat.endpoint;
        if (ep.empty() || ep == "local") {
            ChatRequest req;
            req.messages = {{"system", sys}, {"user", facts_text}};
            req.enable_thinking = false;
            req.sampling.max_tokens = 400;
            req.priority = 2;  // background
            req.tag = "heartbeat";
            reply = engine.chat(req).message.content;
        } else {
            httplib::Client cli(ep.substr(0, ep.find("/v1")).c_str());
            cli.set_read_timeout(120, 0);
            json body = {{"model", cfg.heartbeat.model.empty() ? "main" : cfg.heartbeat.model},
                         {"messages", json::array({{{"role", "system"}, {"content", sys}},
                                                   {{"role", "user"}, {"content", facts_text}}})},
                         {"max_tokens", 400}, {"chat_template_kwargs", {{"enable_thinking", false}}}};
            std::string path = ep.substr(ep.find("/v1") == std::string::npos ? ep.size() : ep.find("/v1"));
            if (path.empty()) path = "/v1";
            auto r = cli.Post(path + "/chat/completions", body.dump(), "application/json");
            if (r && r->status == 200) try {
                    reply = json::parse(r->body)["choices"][0]["message"].value("content", "");
                } catch (...) {
                }
        }
        std::string sev;
        auto p = reply.find("SEVERITY:");
        if (p != std::string::npos) {
            std::string rest = reply.substr(p + 9);
            for (const char * s : {"critical", "warn", "notice", "ok"})
                if (rest.find(s) != std::string::npos && rest.find(s) < 12) { sev = s; break; }
        }
        return {reply, sev};
    }

    // "HH:MM-HH:MM" local; true if now is inside it (handles ranges that cross midnight). Empty = never quiet.
    static bool in_quiet_hours(const std::string & spec) {
        auto dash = spec.find('-');
        if (dash == std::string::npos) return false;
        auto mins = [](const std::string & s) -> int {
            auto c = s.find(':');
            if (c == std::string::npos) return -1;
            try {
                return std::stoi(s.substr(0, c)) * 60 + std::stoi(s.substr(c + 1));
            } catch (...) {
                return -1;
            }
        };
        int a = mins(spec.substr(0, dash)), b = mins(spec.substr(dash + 1));
        if (a < 0 || b < 0) return false;
        std::time_t t = std::time(nullptr);
        std::tm lt{};
        localtime_r(&t, &lt);
        int now = lt.tm_hour * 60 + lt.tm_min;
        return a <= b ? (now >= a && now < b) : (now >= a || now < b);
    }

    void run_heartbeat_loop() {
        while (!stopping.load()) {
            int wait = std::max(30, cfg.heartbeat.interval_s);
            for (int i = 0; i < wait && !stopping.load(); i++) std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stopping.load() || !cfg.heartbeat.enabled) continue;
            try {
                HeartbeatReport rep = run_heartbeat(cfg.heartbeat, [this](const std::string & t) { return summarize(t); });
                // push to a channel when something needs attention (warn or critical), honoring quiet hours
                // (no push during quiet hours unless it is critical)
                bool quiet = in_quiet_hours(cfg.heartbeat.quiet_hours);
                if ((rep.severity == "critical") || (rep.severity == "warn" && !quiet)) {
                    Channels ch(cfg.channels);
                    std::string chan = cfg.heartbeat.channel.empty() ? cfg.channels.def : cfg.heartbeat.channel;
                    if (!ch.names().empty()) rep.pushed = ch.send(chan, "Judas heartbeat: " + rep.severity, rep.summary).ok;
                }
                std::lock_guard lk(hb_mu);
                last_hb = std::move(rep);
            } catch (...) {
            }
        }
    }

    bool auto_resume_on(const std::string & sid) {
        auto row = store.get_session(sid);
        if (!row) return false;
        return row->auto_resume < 0 ? cfg.agents.auto_resume : row->auto_resume > 0;
    }

    // Auto-resume: a paused lead turn (a cap hit with work left) is resumed after auto_resume_delay_s when the
    // session has it on, at most auto_resume_max times a day, and it stops once the last
    // kAutoResumeStallLimit resumes changed no file and ticked no plan item. Failed and killed turns stay put.
    void auto_resume_tick() {
        std::vector<std::string> sids;
        {
            std::lock_guard lk(resume_mu);
            for (auto & [sid, ms] : paused_since) sids.push_back(sid);
        }
        for (auto & sid : sids) {
            auto info = agents.turn_info(sid);
            long long now = now_ms();
            std::lock_guard lk(resume_mu);
            auto ps = paused_since.find(sid);
            if (ps == paused_since.end()) continue;
            if (info.status != "paused") {
                if (info.status != "running") paused_since.erase(ps);   // done / error / killed: nothing to resume
                continue;
            }
            auto & tr = resume_track[sid];
            if (tr.pending && ps->second >= tr.resumed_ms) {   // paused again: judge the stretch it just ran
                tr.stalled = resume_made_progress(tr.writes_at, write_count[sid], tr.open_at, info.open_items) ? 0 : tr.stalled + 1;
                tr.pending = false;
            }
            std::time_t t = (std::time_t)(now / 1000);
            std::tm lt{};
            localtime_r(&t, &lt);
            char day[16];
            std::strftime(day, sizeof day, "%Y-%m-%d", &lt);
            if (tr.day != day) { tr.day = day; tr.attempts = 0; }
            AutoResumeInput in;
            in.enabled = auto_resume_on(sid);
            in.status = info.status;
            in.paused_for_ms = now - ps->second;
            in.delay_s = cfg.agents.auto_resume_delay_s;
            in.max_per_day = cfg.agents.auto_resume_max;
            in.attempts_today = tr.attempts;
            in.stalled = tr.stalled;
            auto act = decide_auto_resume(in);
            if (act != AutoResumeAction::Resume) continue;
            auto row = store.get_session(sid);
            tr.attempts++;
            tr.pending = true;
            tr.writes_at = write_count[sid];
            tr.open_at = info.open_items;
            tr.resumed_ms = now;
            paused_since.erase(ps);
            if (!agents.resume(sid, row ? row->cwd : "")) tr.pending = false;
        }
    }

    void run_auto_resume() {
        int tick = 5;   // seconds between looks at paused turns (JUDAS_AUTO_RESUME_TICK_S, for tests)
        if (const char * e = std::getenv("JUDAS_AUTO_RESUME_TICK_S")) tick = std::clamp(std::atoi(e), 1, 60);
        while (!stopping.load()) {
            for (int i = 0; i < tick && !stopping.load(); i++) std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stopping.load()) break;
            try {
                auto_resume_tick();
            } catch (...) {
            }
        }
    }

    // Fires due scheduled tasks: every 20 s, each enabled task whose next_ms has passed is submitted as a new
    // session, then rescheduled (interval) or disabled (once).
    void run_scheduler() {
        while (!stopping.load()) {
            for (int i = 0; i < 20 && !stopping.load(); i++) std::this_thread::sleep_for(std::chrono::seconds(1));
            if (stopping.load()) break;
            try {
                long long now = now_ms();
                for (auto t : store.due_tasks(now)) {
                    std::string sid = agents.submit("new", t.prompt, t.cwd);
                    mark_task_session(sid);
                    t.last_session = sid;
                    t.last_run_ms = now;
                    if (t.kind == "interval" && t.interval_s > 0) t.next_ms = now + t.interval_s * 1000;
                    else { t.enabled = false; t.next_ms = 0; }  // once: done
                    store.update_task(t);
                }
            } catch (...) {
                // a bad task should not kill the scheduler
            }
            try {
                curator_tick();
            } catch (...) {
                // the curator is housekeeping: its failures must not take the scheduler down
            }
        }
    }

    // Curator hook (docs/LEARNING.md): at most one pass per learning.curator_interval_hours and
    // only after no agent has been active for curator_min_idle_minutes. File work (tar, moves) runs on this
    // scheduler thread; nothing else awaits it. The first pass ever is just the schedule seed, then the
    // interval has to elapse like for any run; a dry run never fires automatically.
    void curator_tick() {
        const LearningConfig & lc = cfg.learning;
        if (!lc.curator) return;
        long long now = now_ms();
        long long active_at = last_agent_active_ms.load(std::memory_order_relaxed);
        if (active_at != 0 && now - active_at < (long long) lc.curator_min_idle_minutes * 60'000) return;
        if (curator_fail_ms != 0 && now - curator_fail_ms < 600'000) return;
        try {
            std::string data = expand_home(cfg.data_dir);
            auto st = read_curator_state(data);
            long long interval_ms = (long long) lc.curator_interval_hours * 3'600'000LL;
            if (st && st->last_run_ms > 0 && now - st->last_run_ms < interval_ms) return;
            Skills skills(cfg.skills);
            SkillUsage usage((std::filesystem::path(data) / "skills.db").string());
            auto r = run_curator(lc, skills.all(), data, skills.write_dir(), usage, now, false);
            if (!r.error.empty()) {
                // A failing pass (say, the tar target is gone) is retried at most every 10 minutes, not per tick.
                curator_fail_ms = now;
                std::fprintf(stderr, "judas curator: %s\n", r.error.c_str());
            } else {
                curator_fail_ms = 0;
                if (r.first_run)
                    std::fprintf(stderr, "judas curator: schedule seeded; first pass comes after one interval\n");
                else
                    std::fprintf(stderr, "judas curator: %s (report %s)\n", r.summary.c_str(), r.report_path.c_str());
            }
        } catch (...) {
        }
    }

    Impl(const Config & c, Engine & e, AgentManager & a, Store & s, Journal & j)
        : cfg(c), engine(e), agents(a), store(s), journal(j) {
        std::vector<std::string> binds = {cfg.server.bind};
        for (auto & b : cfg.server.extra_binds) binds.push_back(b);
        bind_addrs = binds;
        for (size_t i = 0; i < binds.size(); i++) {
            auto s2 = std::make_unique<httplib::Server>();
            setup(*s2);
            servers.push_back(std::move(s2));
        }
        listener_handle = agents.add_listener([this](const AgentEvent & e) { on_agent_event(e); });
        scheduler = std::thread([this] { run_scheduler(); });
        heartbeat_thread = std::thread([this] { run_heartbeat_loop(); });
        resume_thread = std::thread([this] { run_auto_resume(); });
    }

    ~Impl() {
        stopping = true;
        hub.wake();
        agents.remove_listener(listener_handle);
        if (scheduler.joinable()) scheduler.join();
        if (heartbeat_thread.joinable()) heartbeat_thread.join();
        if (resume_thread.joinable()) resume_thread.join();
    }

    std::vector<std::string> bind_addrs;

    // Scheduled and task runs are delegated work: they belong to the Cowork surface.
    void mark_task_session(const std::string & sid) {
        if (store.set_session_mode(sid, "cowork")) announce_session("session.updated", sid);
    }
    void announce_session(const std::string & type, const std::string & sid) {
        auto s = store.get_session(sid);
        if (s) hub.push("*", type, dump_json({{"type", type}, {"session", session_json(*s)}}));
    }
    void on_agent_event(const AgentEvent & e);
    void stream_ring(httplib::Response & res, const std::string & key, uint64_t after, bool close_on_lead_done,
                     std::string synthetic);
    json turn_json(const std::string & sid);
    void setup(httplib::Server & svr);
    std::string host_only(const std::string & h);
    bool host_allowed(const std::string & host);
    bool origin_allowed(const std::string & origin);
    bool same_origin(const std::string & origin, const std::string & host);
    void cors_headers(const httplib::Request & req, httplib::Response & res);
    void chat_completions(const httplib::Request & req, httplib::Response & res);
    void events(const httplib::Request & req, httplib::Response & res);
    void global_events(const httplib::Request & req, httplib::Response & res);
};

void Server::Impl::chat_completions(const httplib::Request & req, httplib::Response & res) {
    json body;
    ChatRequest cr;
    try {
        body = json::parse(req.body);
        cr = parse_chat(body, req.get_header_value("User-Agent"));
    } catch (const std::exception & e) {
        send_json(res, error_json(std::string("bad request: ") + e.what(), "invalid_request_error"), 400);
        return;
    }
    bool stream = body.value("stream", false);
    bool include_usage = body.contains("stream_options") && body["stream_options"].is_object() &&
                         body["stream_options"].value("include_usage", false);
    std::string model = engine.model_name();
    std::string id = "chatcmpl-" + rand_hex(24);
    long created = (long)std::time(nullptr);

    if (!stream) {
        ChatResult r;
        try {
            r = engine.chat(cr);
        } catch (const std::exception & e) {
            send_json(res, error_json(e.what(), "server_error"), 500);
            return;
        }
        if (r.finish_reason == "error") {
            send_json(res, error_json(r.error.empty() ? "generation failed" : r.error, "server_error"), 500);
            return;
        }
        json out = {{"id", id}, {"object", "chat.completion"}, {"created", created}, {"model", model},
                    {"choices", json::array({{{"index", 0}, {"message", message_to_oai(r.message)},
                                              {"finish_reason", finish_of(r)}}})},
                    {"usage", usage_json(r)}};
        send_json(res, out);
        return;
    }

    // by value: the provider below runs after this handler returns, so references here would dangle
    auto chunk = [id, created, model](const json & delta, const json & finish) {
        return json{{"id", id}, {"object", "chat.completion.chunk"}, {"created", created}, {"model", model},
                    {"choices", json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}};
    };
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    // The whole generation runs inside the first provider call, on this handler thread, so on_delta can
    // write straight to the sink. A failed write means the client left: cancel the engine.
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, cr, chunk, include_usage, id, created, model](size_t, httplib::DataSink & sink) mutable {
            std::atomic<bool> cancel{false};
            auto put = [&](const json & j) {
                std::string s = sse(j);
                if (!sink.write(s.data(), s.size())) {
                    cancel = true;
                    return false;
                }
                return true;
            };
            if (!put(chunk({{"role", "assistant"}}, nullptr))) return false;
            ChatResult r;
            try {
                r = engine.chat(
                    cr,
                    [&](const ChatDelta & d) {
                        if (cancel) return false;
                        if (!d.reasoning.empty() && !put(chunk({{"reasoning_content", d.reasoning}}, nullptr)))
                            return false;
                        if (!d.content.empty() && !put(chunk({{"content", d.content}}, nullptr))) return false;
                        return !stopping.load();
                    },
                    &cancel);
            } catch (const std::exception & e) {
                r.finish_reason = "error";
                r.error = e.what();
            }
            if (cancel) {
                sink.done();
                return true;
            }
            if (r.finish_reason == "error") {
                put(error_json(r.error.empty() ? "generation failed" : r.error, "server_error"));
            } else {
                for (size_t i = 0; i < r.message.tool_calls.size(); i++) {
                    auto & tc = r.message.tool_calls[i];
                    json call = {{"index", (int)i}, {"id", tc.id}, {"type", "function"},
                                 {"function", {{"name", tc.name}, {"arguments", tc.arguments}}}};
                    put(chunk({{"tool_calls", json::array({call})}}, nullptr));
                }
                put(chunk(json::object(), finish_of(r)));
                if (include_usage) {
                    json u = {{"id", id}, {"object", "chat.completion.chunk"}, {"created", created},
                              {"model", model}, {"choices", json::array()}, {"usage", usage_json(r)}};
                    put(u);
                }
            }
            std::string done = "data: [DONE]\n\n";
            sink.write(done.data(), done.size());
            sink.done();
            return true;
        });
}

// Every agent event lands in its session's ring (replayable, with ids). A trimmed copy of the ones that
// change the session list or the agent board also goes to the global feed.
void Server::Impl::on_agent_event(const AgentEvent & e) {
    // Any agent chatter counts as engine activity; the curator only runs in the quiet after it.
    last_agent_active_ms.store(now_ms(), std::memory_order_relaxed);
    bool lead = e.parent.empty();
    if (e.type == "tool_call") {
        std::string tn = e.data.value("name", std::string());
        if (tn == "write_file" || tn == "edit_file") {
            std::lock_guard lk(resume_mu);
            write_count[e.session]++;
        }
    } else if (lead && e.type == "status" && e.data.value("turn_status", std::string()) == "paused") {
        std::lock_guard lk(resume_mu);
        paused_since[e.session] = now_ms();
    }
    std::string turn_ev = !lead ? "" : e.type == "spawned" ? "turn_start" : e.type == "done" ? "turn_end" : "turn";
    if (lead && e.type != "done") {
        bool was_open = false;
        hub.replay_floor(e.session, &was_open);
        if (!was_open) {
            long long f = store.last_message_id(e.session);
            std::lock_guard lk(floor_mu);
            msg_floor[e.session] = f;
        }
    }
    hub.push(e.session, e.type, dump_json(event_json(e)), turn_ev);
    if (e.type == "spawned" || e.type == "status" || e.type == "done") {
        std::string st = e.type == "spawned" ? "running" : e.data.value("status", std::string());
        if (e.type == "status" && e.data.contains("error")) st = "failed";
        hub.push("*", "agent.status",
                 dump_json({{"type", "agent.status"}, {"session", e.session}, {"agent", e.agent},
                            {"parent", e.parent}, {"role", e.role}, {"status", st}}));
    }
    if (lead && e.type == "spawned") {
        hub.push("*", "turn.started", dump_json({{"type", "turn.started"}, {"session", e.session}}));
        announce_session("session.updated", e.session);
    } else if (lead && e.type == "done") {
        hub.push("*", "turn.finished",
                 dump_json({{"type", "turn.finished"}, {"session", e.session},
                            {"status", e.data.value("status", std::string("done"))}}));
        announce_session("session.updated", e.session);
    }
}

json Server::Impl::turn_json(const std::string & sid) {
    bool in_progress = false;
    uint64_t floor = hub.replay_floor(sid, &in_progress);
    long long mf = 0;
    if (in_progress) {
        std::lock_guard lk(floor_mu);
        auto it = msg_floor.find(sid);
        if (it != msg_floor.end()) mf = it->second;
    }
    auto ti = agents.turn_info(sid);
    return {{"running", in_progress}, {"event_floor", floor}, {"msg_floor", mf}, {"events_head", hub.head(sid)},
            {"status", ti.status}, {"reason", ti.reason}, {"open_items", ti.open_items},
            {"auto_resume_default", cfg.agents.auto_resume}};
}

// Streams ring `key` as SSE from event id `after` on. Each frame carries `id:` so EventSource resends it as
// Last-Event-ID on reconnect. The handler thread holds no queue: it pulls from the bounded ring.
// close_on_lead_done: legacy mode, end the stream at the first lead "done" (or at a deleted session).
void Server::Impl::stream_ring(httplib::Response & res, const std::string & key, uint64_t after,
                               bool close_on_lead_done, std::string synthetic) {
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Accel-Buffering", "no");
    res.set_chunked_content_provider(
        "text/event-stream",
        [this, key, after, close_on_lead_done, synthetic](size_t, httplib::DataSink & sink) mutable {
            auto last_write = Clock::now();
            auto put = [&](const std::string & s) {
                if (!sink.write(s.data(), s.size())) return false;
                last_write = Clock::now();
                return true;
            };
            if (!synthetic.empty()) {
                if (!put("data: " + synthetic + "\n\n")) return false;
                sink.done();
                return true;
            }
            uint64_t sent = after;
            bool gap_told = false;
            while (!stopping.load()) {
                uint64_t gen = hub.generation();   // read before pulling: a push in between is not missed
                auto b = hub.since(key, sent);
                if (b.gap && !gap_told) {
                    gap_told = true;   // some events were evicted (or the daemon restarted): tell the page once
                    if (!put("data: " + dump_json({{"type", "gap"}, {"session", key}}) + "\n\n")) return false;
                }
                for (auto & e : b.entries) {
                    if (!put("id: " + std::to_string(e.id) + "\ndata: " + e.json + "\n\n")) return false;
                    sent = e.id;
                    if (close_on_lead_done && e.type == "done" && e.json.find("\"parent\":\"\"") != std::string::npos) {
                        sink.done();
                        return true;
                    }
                    if (e.type == "session_deleted") {
                        sink.done();
                        return true;
                    }
                }
                if (!b.entries.empty()) continue;
                hub.wait(gen, 500);
                if (Clock::now() - last_write >= std::chrono::seconds(15)) {
                    if (!put(": ping\n\n")) return false;
                } else if (!sink.is_writable()) {
                    return false;
                }
            }
            sink.done();
            return true;
        });
}

// GET /api/sessions/<id>/events[?since=N][&follow=1]; Last-Event-ID is honored like ?since.
//   no since: replays the turn in progress from its first event (so a refreshed page rebuilds the partial
//   message), then goes live. since=N: replays everything after N (live-only if N is the head).
//   follow=1 keeps the stream open across turns; without it the stream ends at the lead's "done" (the
//   original behavior, which curl/CLI clients rely on).
void Server::Impl::events(const httplib::Request & req, httplib::Response & res) {
    std::string sid = req.matches[1];
    bool follow = req.get_param_value("follow") == "1";
    std::string sv = req.has_param("since") ? req.get_param_value("since") : req.get_header_value("Last-Event-ID");
    bool has_since = !sv.empty() && std::isdigit((unsigned char) sv[0]);
    uint64_t after = 0;
    std::string synthetic;
    if (has_since) {
        after = std::strtoull(sv.c_str(), nullptr, 10);
    } else {
        bool in_progress = false;
        after = hub.replay_floor(sid, &in_progress);
        if (!in_progress && !follow) {
            // The lead may have finished before we subscribed: answer with a synthetic done event.
            for (auto & a : agents.agents(sid))
                if (a.parent.empty() && (a.status == "done" || a.status == "failed" || a.status == "killed" || a.status == "paused")) {
                    AgentEvent e{"done", sid, a.id, "", a.role, {{"result", a.result}, {"status", a.status}}};
                    if (a.status == "paused" || a.status == "failed") e.data["reason"] = agents.turn_info(sid).reason;
                    synthetic = dump_json(event_json(e));
                }
        }
    }
    stream_ring(res, sid, after, !follow, synthetic);
}

// GET /api/events[?since=N]: session list and agent board changes across all sessions (the page keeps this
// open instead of polling). Live-only unless since/Last-Event-ID is given.
void Server::Impl::global_events(const httplib::Request & req, httplib::Response & res) {
    std::string sv = req.has_param("since") ? req.get_param_value("since") : req.get_header_value("Last-Event-ID");
    uint64_t after = !sv.empty() && std::isdigit((unsigned char) sv[0]) ? std::strtoull(sv.c_str(), nullptr, 10)
                                                                       : hub.head("*");
    stream_ring(res, "*", after, false, "");
}

std::string Server::Impl::host_only(const std::string & h) {
    if (h.empty()) return "";
    if (h[0] == '[') return h.substr(1, h.find(']') - 1);   // [::1]:8079
    size_t c = h.rfind(':');
    std::string out = (c != std::string::npos && h.find(':') == c) ? h.substr(0, c) : h;
    for (auto & ch : out) ch = (char) std::tolower((unsigned char) ch);
    return out;
}

bool Server::Impl::host_allowed(const std::string & host) {
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" || host == cfg.server.bind) return true;
    for (auto & b : cfg.server.extra_binds) if (host == b) return true;
    for (auto & a : cfg.server.allowed_hosts) if (host == host_only(a) || a == "*") return true;
    char hn[256] = {0};
    if (::gethostname(hn, sizeof hn - 1) == 0) {
        std::string me = host_only(hn);
        if (host == me || host == me + ".local") return true;
    }
    // a bare IP literal of this machine bound on 0.0.0.0
    return cfg.server.bind == "0.0.0.0" && !host.empty() && std::isdigit((unsigned char) host[0]) &&
           host.find_first_not_of("0123456789.") == std::string::npos;
}

bool Server::Impl::origin_allowed(const std::string & origin) {
    for (auto & o : cfg.server.cors_origins) if (o == "*" || o == origin) return true;
    return false;
}

bool Server::Impl::same_origin(const std::string & origin, const std::string & host) {
    return origin == "http://" + host || origin == "https://" + host;
}

void Server::Impl::cors_headers(const httplib::Request & req, httplib::Response & res) {
    std::string origin = req.get_header_value("Origin");
    if (origin.empty() || !origin_allowed(origin)) return;
    res.set_header("Access-Control-Allow-Origin", origin);
    res.set_header("Vary", "Origin");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS");
}

void Server::Impl::setup(httplib::Server & svr) {
    svr.new_task_queue = [] { return new httplib::ThreadPool(32, http_max_threads()); };
    svr.set_payload_max_length(64u * 1024 * 1024);

    // There is no auth, so the browser is the threat model: a page on another site must not be able to drive
    // the API. Host must name this machine (DNS rebinding), cross-origin writes need a listed origin (a plain
    // form POST skips the CORS preflight), and CORS headers go only to listed origins.
    svr.set_pre_routing_handler([this](const httplib::Request & req, httplib::Response & res) {
        t_start = Clock::now();
        std::string host = host_only(req.get_header_value("Host"));
        if (!host.empty() && !host_allowed(host)) {
            send_json(res, error_json("host '" + host + "' is not allowed; add it to server.allowed_hosts", "forbidden"), 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        std::string origin = req.get_header_value("Origin");
        bool safe = req.method == "GET" || req.method == "HEAD" || req.method == "OPTIONS";
        if (!safe && !origin.empty() && !same_origin(origin, req.get_header_value("Host")) && !origin_allowed(origin)) {
            send_json(res, error_json("origin '" + origin + "' may not call this API; add it to server.cors_origins", "forbidden"), 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    svr.set_post_routing_handler([this](const httplib::Request & req, httplib::Response & res) {
        if (req.path.rfind("/v1", 0) == 0 || req.path.rfind("/api", 0) == 0) cors_headers(req, res);
    });
    svr.set_logger([](const httplib::Request & req, const httplib::Response & res) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_start).count();
        std::fprintf(stderr, "%s %s %d %lldms\n", req.method.c_str(), req.path.c_str(), res.status, (long long)ms);
    });
    svr.set_exception_handler([](const httplib::Request &, httplib::Response & res, std::exception_ptr ep) {
        std::string msg = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception & e) {
            msg = e.what();
        } catch (...) {
        }
        send_json(res, error_json(msg, "server_error"), 500);
    });
    svr.set_error_handler([](const httplib::Request &, httplib::Response & res) {
        if (res.body.empty()) send_json(res, error_json("not found", "not_found"), res.status);
    });

    svr.Options(R"(.*)", [this](const httplib::Request & req, httplib::Response & res) {
        res.status = 204;
        cors_headers(req, res);
    });

    svr.Get("/health", [this](const httplib::Request &, httplib::Response & res) {
        send_json(res, {{"status", "ok"}, {"model", engine.model_name()}});
    });
    auto state_handler = [this](const httplib::Request &, httplib::Response & res) {
        json j = state_json(engine.state());
        json ss = json::array();
        for (auto & s : store.list_sessions(10))
            ss.push_back({{"id", s.id}, {"title", s.title}, {"updated_ms", s.updated_ms}});
        j["sessions"] = ss;
        j["version"] = version();
        send_json(res, j);
    };
    svr.Get("/state", state_handler);
    svr.Get("/api/state", state_handler);

    svr.Get("/v1/models", [this](const httplib::Request &, httplib::Response & res) {
        json data = json::array();
        std::string name = engine.model_name();
        // The stock GGUF name first (what clients expect to see), then the config key; any id is accepted.
        std::string stock;
        if (auto it = cfg.models.find(name); it != cfg.models.end() && !it->second.path.empty())
            stock = gguf_display_name(it->second.path);
        for (auto & id : {stock, name, std::string("main")}) {
            if (id.empty()) continue;
            bool dup = false;
            for (auto & d : data) dup = dup || d["id"] == id;
            if (!dup) data.push_back({{"id", id}, {"object", "model"}, {"created", 0}, {"owned_by", "judas"}});
        }
        send_json(res, {{"object", "list"}, {"data", data}});
    });
    svr.Post("/v1/chat/completions",
             [this](const httplib::Request & req, httplib::Response & res) { chat_completions(req, res); });

    // Judas API
    svr.Get("/api/sessions", [this](const httplib::Request & req, httplib::Response & res) {
        // ?limit= (default 200, max 1000) and ?before=<updated_ms of the last row seen> page through the list
        size_t limit = 200;
        if (req.has_param("limit")) limit = (size_t) std::clamp(std::atoi(req.get_param_value("limit").c_str()), 1, 1000);
        long long before = req.has_param("before") ? std::atoll(req.get_param_value("before").c_str()) : 0;
        auto rows = store.list_sessions(limit + 1, before);
        bool more = rows.size() > limit;
        if (more) rows.resize(limit);
        json a = json::array();
        for (auto & s : rows) {
            json sj = session_json(s);
            bool in_progress = false;
            hub.replay_floor(s.id, &in_progress);
            sj["running"] = in_progress;   // a turn is in progress (so the list can show it before any event)
            auto ti = agents.turn_info(s.id);
            sj["turn_status"] = ti.status;   // idle | running | paused | done | error | killed
            sj["turn_reason"] = ti.reason;
            sj["open_items"] = ti.open_items;
            a.push_back(sj);
        }
        send_json(res, {{"sessions", a}, {"more", more}, {"auto_resume_default", cfg.agents.auto_resume}});
    });
    svr.Post("/api/sessions", [this](const httplib::Request & req, httplib::Response & res) {
        json b = json::object();
        if (!req.body.empty()) b = json::parse(req.body, nullptr, false);
        if (!b.is_object()) return send_json(res, error_json("bad json", "invalid_request_error"), 400);
        std::string mode = b.value("mode", "chat");
        if (!valid_mode(mode)) return send_json(res, error_json("mode must be chat, cowork or code", "invalid_request_error"), 400);
        std::string cwd = b.value("cwd", "");
        if (cwd.empty()) cwd = std::filesystem::current_path().string();
        auto s = store.create_session(b.value("title", "New session"), cwd, engine.model_name(), mode);
        announce_session("session.created", s.id);
        send_json(res, session_json(s));
    });
    // Change a session's mode (chat | cowork | code) and/or title.
    svr.Patch(R"(/api/sessions/([^/]+))", [this](const httplib::Request & req, httplib::Response & res) {
        std::string id = req.matches[1];
        json b = json::parse(req.body, nullptr, false);
        if (!b.is_object()) return send_json(res, error_json("bad json", "invalid_request_error"), 400);
        if (!store.get_session(id)) return send_json(res, error_json("no such session", "not_found"), 404);
        if (b.contains("mode")) {
            if (!b["mode"].is_string() || !valid_mode(b["mode"]))
                return send_json(res, error_json("mode must be chat, cowork or code", "invalid_request_error"), 400);
            store.set_session_mode(id, b["mode"]);
        }
        if (b.contains("title") && b["title"].is_string() && !b["title"].get<std::string>().empty())
            store.rename_session(id, b["title"]);
        if (b.contains("auto_resume")) {   // true | false | null (null = follow agents.auto_resume)
            const json & v = b["auto_resume"];
            if (!v.is_boolean() && !v.is_null())
                return send_json(res, error_json("auto_resume must be true, false or null", "invalid_request_error"), 400);
            store.set_session_auto_resume(id, v.is_null() ? -1 : v.get<bool>() ? 1 : 0);
        }
        announce_session("session.updated", id);
        send_json(res, session_json(*store.get_session(id)));
    });
    // One session: rows, agents, a page of messages across all agents (oldest first).
    //   ?limit=N returns the newest N (with ?before=<message id> the N before that id); no limit = everything.
    //   The reply also has more_before, total and `turn` (a turn in progress: the event id and message id to
    //   resume from, see /events).
    svr.Get(R"(/api/sessions/([^/]+))", [this](const httplib::Request & req, httplib::Response & res) {
        auto s = store.get_session(req.matches[1]);
        if (!s) return send_json(res, error_json("no such session", "not_found"), 404);
        size_t limit = req.has_param("limit") ? (size_t) std::clamp(std::atoi(req.get_param_value("limit").c_str()), 0, 5000) : 0;
        long long before = req.has_param("before") ? std::atoll(req.get_param_value("before").c_str()) : 0;
        bool more = false;
        long long total = 0;
        json ag = json::array(), ms = json::array();
        bool show_all = req.get_param_value("all") == "1";   // hidden review agents only with ?all=1
        for (auto & a : store.list_agents(s->id))
            if (show_all || a.role != "review") ag.push_back(agent_json(a));
        for (auto & m : store.messages_page(s->id, before, limit, &more, &total))
            ms.push_back({{"id", m.id}, {"agent", m.agent}, {"role", m.role}, {"body", m.body},
                          {"created_ms", m.created_ms}});
        send_json(res, {{"session", session_json(*s)}, {"agents", ag}, {"messages", ms},
                        {"more_before", more}, {"total", total}, {"turn", turn_json(s->id)}});
    });
    svr.Post(R"(/api/sessions/([^/]+)/messages)", [this](const httplib::Request & req, httplib::Response & res) {
        json b = json::parse(req.body, nullptr, false);
        if (!b.is_object() || !b.contains("content") || !b["content"].is_string())
            return send_json(res, error_json("body needs content", "invalid_request_error"), 400);
        std::string id = req.matches[1];
        std::string cwd = b.value("cwd", "");
        std::string mode = b.contains("mode") && b["mode"].is_string() ? b["mode"].get<std::string>() : "";
        if (!mode.empty() && !valid_mode(mode))
            return send_json(res, error_json("mode must be chat, cowork or code", "invalid_request_error"), 400);
        if (id == "new") {
            std::string text = b["content"];
            std::string title = text.substr(0, 60);
            for (auto & c : title) if (c == '\n') c = ' ';
            if (cwd.empty()) cwd = std::filesystem::current_path().string();   // store the effective folder
            id = store.create_session(title, cwd, engine.model_name(), mode.empty() ? "chat" : mode).id;
            announce_session("session.created", id);
        } else if (auto ex = store.get_session(id)) {
            if (cwd.empty()) cwd = ex->cwd.empty() ? std::filesystem::current_path().string() : ex->cwd;
            if (!mode.empty() && mode != ex->mode) {
                store.set_session_mode(id, mode);
                announce_session("session.updated", id);
            }
        } else {
            return send_json(res, error_json("no such session", "not_found"), 404);
        }
        std::string sid = agents.submit(id, b["content"], cwd);
        send_json(res, {{"session", sid}});
    });
    // Learn: rewrite the request into the shared /learn prompt and start it as a normal turn. A missing
    // session creates one, so the TUI and web chat keep their usual turn and event handling.
    svr.Post("/api/learn", [this](const httplib::Request & req, httplib::Response & res) {
        json b = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
        if (!b.is_object()) return send_json(res, error_json("bad json", "invalid_request_error"), 400);
        if (b.contains("request") && !b["request"].is_string())
            return send_json(res, error_json("request must be a string", "invalid_request_error"), 400);
        if (b.contains("session") && !b["session"].is_string())
            return send_json(res, error_json("session must be a string", "invalid_request_error"), 400);
        if (b.contains("cwd") && !b["cwd"].is_string())
            return send_json(res, error_json("cwd must be a string", "invalid_request_error"), 400);
        std::string request = b.value("request", "");
        std::string cwd = b.value("cwd", "");
        std::string id = b.value("session", "");
        if (id.empty()) {
            std::string title = request.empty() ? "Learn from this conversation" : "Learn: " + request.substr(0, 53);
            for (auto & c : title) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (cwd.empty()) cwd = std::filesystem::current_path().string();
            id = store.create_session(title, cwd, engine.model_name(), "chat").id;
            announce_session("session.created", id);
        } else if (auto ex = store.get_session(id)) {
            if (cwd.empty()) cwd = ex->cwd.empty() ? std::filesystem::current_path().string() : ex->cwd;
        } else {
            return send_json(res, error_json("no such session", "not_found"), 404);
        }
        std::string sid = agents.submit(id, build_learn_prompt(request), cwd);
        send_json(res, {{"session", sid}});
    });
    svr.Get(R"(/api/sessions/([^/]+)/events)",
            [this](const httplib::Request & req, httplib::Response & res) { events(req, res); });
    svr.Get("/api/events", [this](const httplib::Request & req, httplib::Response & res) { global_events(req, res); });
    svr.Post(R"(/api/sessions/([^/]+)/cancel)", [this](const httplib::Request & req, httplib::Response & res) {
        agents.cancel(req.matches[1]);
        send_json(res, {{"ok", true}});
    });
    // Resume a paused turn (a cap was hit with work left). 409 when the session has no paused turn.
    svr.Post(R"(/api/sessions/([^/]+)/resume)", [this](const httplib::Request & req, httplib::Response & res) {
        std::string id = req.matches[1];
        auto s = store.get_session(id);
        if (!s) return send_json(res, error_json("no such session", "not_found"), 404);
        if (!agents.resume(id, s->cwd))
            return send_json(res, error_json("the session has no paused turn to resume", "conflict"), 409);
        send_json(res, {{"ok", true}, {"session", id}});
    });
    // Recover: undo every change this chat made (revert all its snapshots).
    svr.Post(R"(/api/sessions/([^/]+)/recover)", [this](const httplib::Request & req, httplib::Response & res) {
        agents.cancel(req.matches[1]);
        RevertResult r = journal.revert_session(req.matches[1]);
        send_json(res, revert_json(r));
    });
    // Seal: keep the current result, drop this chat's step-by-step undo history to save disk, end the chat.
    svr.Post(R"(/api/sessions/([^/]+)/seal)", [this](const httplib::Request & req, httplib::Response & res) {
        agents.cancel(req.matches[1]);
        size_t freed = journal.seal_session(req.matches[1]);
        send_json(res, {{"ok", true}, {"freed_bytes", freed}});
    });
    // Delete a chat for good: stop its agents, reclaim its snapshot disk, drop its rows. Files on disk from
    // the chat's work are left as they are (delete is not revert; use /recover to undo changes first).
    svr.Delete(R"(/api/sessions/([^/]+))", [this](const httplib::Request & req, httplib::Response & res) {
        std::string id = req.matches[1];
        if (!store.get_session(id)) return send_json(res, error_json("no such session", "not_found"), 404);
        // Cancel, then wait (bounded) for the session's agent threads to stop: they write messages, and a
        // row deleted under a running agent makes add_message fail on the foreign key.
        auto idle = [&] {
            for (auto & a : agents.agents(id))
                if (a.status == "running") return false;
            return true;
        };
        auto deadline = Clock::now() + std::chrono::seconds(15);
        agents.cancel(id);
        while (!idle() && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            agents.cancel(id);
        }
        if (!idle())
            return send_json(res, error_json("the chat's agents are still stopping; try again in a moment", "conflict"), 409);
        size_t freed = journal.seal_session(id);
        store.delete_session(id);
        hub.push(id, "session_deleted", dump_json({{"type", "session_deleted"}, {"session", id}}));
        hub.push("*", "session.deleted", dump_json({{"type", "session.deleted"}, {"id", id}}));
        {
            std::lock_guard lk(floor_mu);
            msg_floor.erase(id);
        }
        send_json(res, {{"ok", true}, {"freed_bytes", freed}});
    });

    // Memory (the Memory screen): the shared memory file as Judas sees it, what goes in the prompt, the bullets
    // Judas wrote, and the skills it has learned. Removing is limited to [judas] bullets.
    svr.Get("/api/memory", [this](const httplib::Request &, httplib::Response & res) {
        namespace fs = std::filesystem;
        std::string path = expand_home(cfg.memory.path);
        json out = {{"path", cfg.memory.path}, {"exists", false}, {"inject_sections", cfg.memory.inject_sections},
                    {"max_memory_tokens", cfg.prompt.max_memory_tokens}};
        std::error_code ec;
        if (path.empty() || !fs::exists(path, ec)) return send_json(res, out);
        std::ifstream f(path, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(f)), {});
        auto mt = fs::last_write_time(path, ec);
        long long mtime_ms = ec ? 0 : (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::file_clock::to_sys(mt).time_since_epoch()).count();
        auto lower = [](std::string x) { for (auto & c : x) c = (char) std::tolower((unsigned char) c); return x; };
        std::vector<std::string> inj;
        for (auto & n : cfg.memory.inject_sections) inj.push_back(lower(n));
        json secs = json::array(), mine = json::array();
        int bullets_total = 0;
        for (auto & name : memory_section_names(text)) {
            std::string body = memory_section_text(text, name);
            std::istringstream is(body);
            std::string l;
            int bullets = 0, judas = 0;
            while (std::getline(is, l)) {
                if (l.rfind("- ", 0) != 0) continue;
                bullets++;
                if (memory_is_judas_line(l)) {
                    judas++;
                    mine.push_back({{"section", name}, {"line", l}});
                }
            }
            bullets_total += bullets;
            bool injected = std::find(inj.begin(), inj.end(), lower(name)) != inj.end();
            secs.push_back({{"name", name}, {"bullets", bullets}, {"judas", judas}, {"bytes", body.size()},
                            {"injected", injected}, {"text", body}});
        }
        std::string block = memory_prompt_block(cfg);
        out["exists"] = true;
        out["bytes"] = text.size();
        out["mtime_ms"] = mtime_ms;
        out["bullets"] = bullets_total;
        out["sections"] = secs;
        out["judas_lines"] = mine;
        out["prompt_block"] = block;
        out["prompt_tokens"] = (block.size() + 3) / 4;
        out["truncated"] = block.find("[memory truncated") != std::string::npos;
        send_json(res, out);
    });
    svr.Post("/api/memory/forget", [this](const httplib::Request & req, httplib::Response & res) {
        json b = json::parse(req.body, nullptr, false);
        if (b.is_discarded() || !b.contains("line") || !b["line"].is_string())
            return send_json(res, error_json("body must be {\"line\": \"- ... [judas] ...\"}", "bad_request"), 400);
        std::string why = memory_forget(cfg, b["line"].get<std::string>());
        if (!why.empty()) return send_json(res, error_json(why, "conflict"), 409);
        send_json(res, {{"ok", true}});
    });
    svr.Get("/api/skills", [this](const httplib::Request &, httplib::Response & res) {
        namespace fs = std::filesystem;
        json out = {{"enabled", cfg.skills.enabled}, {"autoload", cfg.skills.autoload}, {"manage", cfg.skills.manage}};
        Skills skills(cfg.skills);
        std::map<std::string, SkillUsageRow> use;
        if (cfg.skills.track_usage) {
            SkillUsage usage((fs::path(expand_home(cfg.data_dir)) / "skills.db").string());
            for (auto & r : usage.all()) use[r.name] = r;
        }
        json a = json::array();
        for (auto & sk : skills.all()) {
            json j = {{"name", sk.name}, {"description", sk.description}, {"triggers", sk.triggers},
                      {"body", sk.body}, {"path", sk.path}, {"pinned", sk.pinned}, {"source", sk.source},
                      {"version", sk.version}};
            auto it = use.find(sk.name);
            if (it != use.end())
                j["usage"] = {{"loads", it->second.loads}, {"autoloads", it->second.autoloads},
                              {"patches", it->second.patches}, {"created_ms", it->second.created_ms},
                              {"last_used_ms", it->second.last_used_ms}, {"state", it->second.state}};
            a.push_back(j);
        }
        out["skills"] = a;
        auto names_in = [](const fs::path & d) {
            json n = json::array();
            std::error_code ec;
            if (fs::is_directory(d, ec))
                for (auto & e : fs::directory_iterator(d, ec))
                    if (e.path().extension() == ".md") n.push_back(e.path().stem().string());
            return n;
        };
        fs::path dir = skills.write_dir();
        out["dir"] = dir.string();
        out["archived"] = names_in(dir / ".archive");
        out["quarantined"] = names_in(dir / ".quarantine");
        send_json(res, out);
    });

    // Model switcher: one model is loaded at a time, so a switch persists the choice and restarts the daemon.
    // A CPU instance (its loaded model has gpu_layers = 0) adds new models as CPU too, so it never grabs the GPU.
    auto cpu_instance = [this] {
        auto it = cfg.models.find(cfg.engine.model);
        return it != cfg.models.end() && it->second.url.empty() && it->second.gpu_layers == 0;
    };
    // Every model on disk is listed: configured ones by name, plus any GGUF the scan finds (your model dirs,
    // LM Studio, the Hugging Face cache, Ollama). Picking an unconfigured file adds a [models.x] block first.
    svr.Get("/api/models", [this, cpu_instance](const httplib::Request &, httplib::Response & res) {
        json a = json::array();
        uint64_t vram = gpu_vram_total();
        bool cpu = cpu_instance();
        std::set<std::string> listed;
        for (auto & m : scan_models(cfg)) {
            if (m.projector || (m.helper && m.config_name.empty())) continue;
            json j = {{"name", m.config_name}, {"path", m.path}, {"label", gguf_display_name(m.label)}, {"source", m.source},
                      {"bytes", m.bytes}, {"size", human_bytes(m.bytes)}, {"fit", cpu ? "cpu" : fit_verdict(m.bytes, vram)},
                      {"arch", m.arch}, {"quant", m.quant}, {"usable", m.ok}, {"error", m.error},
                      {"configured", !m.config_name.empty()}, {"active", m.config_name == cfg.engine.model}};
            if (auto it = cfg.models.find(m.config_name); it != cfg.models.end())
                j.update({{"gpu_layers", it->second.gpu_layers}, {"ctx_total", it->second.ctx_total}});
            if (!m.config_name.empty()) listed.insert(m.config_name);
            a.push_back(j);
        }
        // remote models (url = ...) have no file to scan
        for (auto & [name, m] : cfg.models)
            if (!listed.count(name) && !m.url.empty())
                a.push_back({{"name", name}, {"path", m.url}, {"label", name}, {"source", "remote"},
                             {"usable", true}, {"configured", true}, {"active", name == cfg.engine.model},
                             {"gpu_layers", m.gpu_layers}, {"ctx_total", m.ctx_total}});
        send_json(res, {{"models", a}, {"active", cfg.engine.model}, {"loaded", engine.model_name()}, {"cpu", cpu}});
    });
    svr.Post("/api/model", [this, cpu_instance](const httplib::Request & req, httplib::Response & res) {
        json b = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
        if (!b.is_object() || !b.contains("name") || !b["name"].is_string())
            return send_json(res, error_json("body needs name", "invalid_request_error"), 400);
        std::string name = b["name"];
        std::string path = user_config_path();
        bool added = false;
        if (!cfg.models.count(name)) {
            // Not configured: it must be a file the scan found (never an arbitrary path from the request).
            const GgufInfo * hit = nullptr;
            auto found = scan_models(cfg);
            for (auto & m : found)
                if (!m.projector && !m.helper && m.config_name.empty() && m.path == name) hit = &m;
            if (!hit) return send_json(res, error_json("no such model: " + name, "invalid_request_error"), 400);
            if (!hit->ok) return send_json(res, error_json("cannot use " + hit->path + ": " + hit->error, "invalid_request_error"), 400);
            std::string base = suggest_model_name(hit->label), pick = base;
            for (int n = 2; cfg.models.count(pick); n++) pick = base + "-" + std::to_string(n);
            std::string err = add_model_to_config(path, pick, *hit, gpu_vram_total(), cpu_instance());
            if (!err.empty()) return send_json(res, error_json(err, "internal_error"), 500);
            name = pick;
            added = true;
        }
        if (!set_engine_model_in_file(path, name))
            return send_json(res, error_json("could not write " + path, "internal_error"), 500);
        bool restarting = false;
        if (added || name != cfg.engine.model || engine.model_name() != name) {
            // INVOCATION_ID alone is not enough: some desktops launch every app as a systemd unit, so a terminal
            // started from the launcher carries it too. We are the service only if our cgroup is judas.service.
            std::ifstream cg("/proc/self/cgroup");
            std::string cgs((std::istreambuf_iterator<char>(cg)), std::istreambuf_iterator<char>());
            if (cgs.find("/judas.service") != std::string::npos) {
                // Running under systemd: restart after the response flushes so the new model loads.
                std::system("(sleep 1; systemctl --user restart judas.service) >/dev/null 2>&1 &");
            } else {
                // Started by hand or from the launcher: re-exec ourselves with the same arguments once the
                // response is out. Every fd but stdio is closed first so the port and the GPU are free.
                std::thread([] {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    std::vector<std::string> args;
                    std::ifstream f("/proc/self/cmdline", std::ios::binary);
                    for (std::string a; std::getline(f, a, '\0');) args.push_back(a);
                    if (args.empty()) return;
                    std::vector<char *> argv;
                    for (auto & a : args) argv.push_back(a.data());
                    argv.push_back(nullptr);
                    close_range(3, ~0U, 0);
                    execv("/proc/self/exe", argv.data());
                    _exit(1);  // exec failed and our sockets are gone: exit rather than linger half-dead
                }).detach();
            }
            restarting = true;
        }
        send_json(res, {{"ok", true}, {"model", name}, {"added", added}, {"restarting", restarting},
                        {"restart_cmd", "systemctl --user restart judas.service"}});
    });

    // Heartbeat: the latest report, and a run-now. Facts are read-only; the summary is the model's.
    auto hb_json = [this] {
        std::lock_guard lk(hb_mu);
        return json{{"summary", last_hb.summary}, {"severity", last_hb.severity}, {"facts", last_hb.facts},
                    {"ran_ms", last_hb.ran_ms}, {"pushed", last_hb.pushed},
                    {"enabled", cfg.heartbeat.enabled}, {"interval_s", cfg.heartbeat.interval_s}};
    };
    svr.Get("/api/heartbeat", [this, hb_json](const httplib::Request &, httplib::Response & res) {
        send_json(res, hb_json());
    });
    svr.Post("/api/heartbeat/run", [this, hb_json](const httplib::Request &, httplib::Response & res) {
        HeartbeatReport rep = run_heartbeat(cfg.heartbeat, [this](const std::string & t) { return summarize(t); });
        {
            std::lock_guard lk(hb_mu);
            last_hb = rep;
        }
        send_json(res, hb_json());
    });
    // Channels: list names, and send a test message (so Settings can confirm Matrix/ntfy works).
    svr.Get("/api/channels", [this](const httplib::Request &, httplib::Response & res) {
        Channels ch(cfg.channels);
        json names = json::array();
        for (auto & n : ch.names()) names.push_back(n);
        send_json(res, {{"channels", names}, {"default", cfg.channels.def}});
    });
    svr.Post("/api/channels/test", [this](const httplib::Request & req, httplib::Response & res) {
        json b = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
        Channels ch(cfg.channels);
        auto r = ch.send(b.value("channel", ""), "Judas test",
                         "Test message from Judas. If you see this, the channel works.");
        send_json(res, {{"ok", r.ok}, {"error", r.error}});
    });

    // Usage statistics (the Statistics screen)
    svr.Get("/api/stats", [this](const httplib::Request & req, httplib::Response & res) {
        int days = req.has_param("days") ? std::max(1, std::atoi(req.get_param_value("days").c_str())) : 371;
        json s = store.usage_stats(days);
        s["model"] = engine.model_name();
        send_json(res, s);
    });

    // Scheduled tasks (the Tasks surface): saved prompts that run on demand, once at a time, or on an interval.
    auto task_json = [](const Store::TaskRow & t) {
        return json{{"id", t.id}, {"title", t.title}, {"prompt", t.prompt}, {"cwd", t.cwd}, {"kind", t.kind},
                    {"run_at_ms", t.run_at_ms}, {"interval_s", t.interval_s}, {"next_ms", t.next_ms},
                    {"enabled", t.enabled}, {"last_session", t.last_session}, {"last_run_ms", t.last_run_ms},
                    {"created_ms", t.created_ms}};
    };
    svr.Get("/api/tasks", [this, task_json](const httplib::Request &, httplib::Response & res) {
        json a = json::array();
        for (auto & t : store.list_tasks()) a.push_back(task_json(t));
        send_json(res, {{"tasks", a}});
    });
    svr.Post("/api/tasks", [this, task_json](const httplib::Request & req, httplib::Response & res) {
        json b = json::parse(req.body, nullptr, false);
        if (!b.is_object() || !b.contains("prompt")) return send_json(res, error_json("body needs prompt", "invalid_request_error"), 400);
        Store::TaskRow t;
        t.title = b.value("title", std::string(b.value("prompt", "")).substr(0, 60));
        t.prompt = b.value("prompt", "");
        t.cwd = b.value("cwd", "");
        t.kind = b.value("kind", "manual");  // manual | once | interval
        t.run_at_ms = b.value("run_at_ms", 0LL);
        t.interval_s = b.value("interval_s", 0LL);
        t.enabled = b.value("enabled", true);
        send_json(res, task_json(store.create_task(t)));
    });
    svr.Post(R"(/api/tasks/([^/]+)/run)", [this, task_json](const httplib::Request & req, httplib::Response & res) {
        auto t = store.get_task(req.matches[1]);
        if (!t) return send_json(res, error_json("no such task", "not_found"), 404);
        std::string sid = agents.submit("new", t->prompt, t->cwd);
        mark_task_session(sid);
        t->last_session = sid;
        t->last_run_ms = now_ms();
        store.update_task(*t);
        send_json(res, {{"session", sid}, {"task", task_json(*t)}});
    });
    svr.Post(R"(/api/tasks/([^/]+)/toggle)", [this, task_json](const httplib::Request & req, httplib::Response & res) {
        auto t = store.get_task(req.matches[1]);
        if (!t) return send_json(res, error_json("no such task", "not_found"), 404);
        t->enabled = !t->enabled;
        if (t->enabled && t->kind == "interval" && t->next_ms == 0) t->next_ms = now_ms() + t->interval_s * 1000;
        store.update_task(*t);
        send_json(res, task_json(*t));
    });
    svr.Delete(R"(/api/tasks/([^/]+))", [this](const httplib::Request & req, httplib::Response & res) {
        store.delete_task(req.matches[1]);
        send_json(res, {{"ok", true}});
    });

    // Snapshots
    svr.Get("/api/snapshots", [this](const httplib::Request & req, httplib::Response & res) {
        size_t limit = 50;
        if (req.has_param("limit")) limit = (size_t)std::max(1, std::atoi(req.get_param_value("limit").c_str()));
        json a = json::array();
        for (auto & c : journal.list(req.get_param_value("session"), limit)) a.push_back(checkpoint_json(c));
        send_json(res, {{"snapshots", a}});
    });
    svr.Get(R"(/api/snapshots/([^/]+)/diff)", [this](const httplib::Request & req, httplib::Response & res) {
        std::string id = req.matches[1];
        if (!journal.get(id)) return send_json(res, error_json("no such snapshot", "not_found"), 404);
        res.set_content(journal.diff(id), "text/plain; charset=utf-8");
    });
    svr.Post(R"(/api/snapshots/([^/]+)/revert)", [this](const httplib::Request & req, httplib::Response & res) {
        std::string id = req.matches[1];
        json b = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
        if (!b.is_object()) return send_json(res, error_json("bad json", "invalid_request_error"), 400);
        if (!journal.get(id)) return send_json(res, error_json("no such snapshot", "not_found"), 404);
        RevertResult r = b.value("to", false) ? journal.revert_to(id) : journal.revert(id);
        send_json(res, revert_json(r));
    });

    // --- web app (embedded) and skins ---
    // Active skin: JUDAS_SKIN env, else "judas". Served as CSS the page imports (the single source of color).
    svr.Get("/skin.css", [](const httplib::Request & req, httplib::Response & res) {
        const char * env = std::getenv("JUDAS_SKIN");
        std::string name = req.has_param("name") ? req.get_param_value("name") : (env ? env : "judas");
        res.set_content(skin_to_css(skin_by_name(name)), "text/css; charset=utf-8");
    });
    svr.Get("/api/skins", [](const httplib::Request &, httplib::Response & res) {
        json arr = json::array();
        for (const auto & s : load_skins()) {
            json c = json::object();
            for (const auto & [k, v] : s.colors) c[k] = v;
            arr.push_back({{"name", s.name}, {"label", s.label}, {"dark", s.dark}, {"colors", c}});
        }
        res.set_content(json({{"skins", arr}}).dump(), "application/json");
    });
    // Static files from the embedded table; "/" serves index.html. Registered last so /api and /v1 win.
    svr.Get(R"((/.*))", [](const httplib::Request & req, httplib::Response & res) {
        std::string path = req.matches[1];
        if (path == "/") path = "/index.html";
        size_t n = 0;
        const Asset * t = assets_table(n);
        for (size_t i = 0; i < n; i++)
            if (path == t[i].path) {
                res.set_content(reinterpret_cast<const char *>(t[i].data), t[i].size, t[i].mime);
                // no-cache: a stale app.js after an upgrade broke sends; assets are tiny and local
                res.set_header("Cache-Control", "no-cache");
                return;
            }
        // SPA fallback: unknown non-asset path returns index.html if we have one, else 404.
        for (size_t i = 0; i < n; i++)
            if (std::string(t[i].path) == "/index.html") {
                res.set_content(reinterpret_cast<const char *>(t[i].data), t[i].size, t[i].mime);
                return;
            }
        res.status = 404;
        res.set_content("not found", "text/plain");
    });
}

Server::Server(const Config & cfg, Engine & engine, AgentManager & agents, Store & store, Journal & journal)
    : impl_(std::make_unique<Impl>(cfg, engine, agents, store, journal)) {}

Server::~Server() { stop(); }

bool Server::run() {
    auto & I = *impl_;
    // The primary bind (server.bind, normally 127.0.0.1) must work: failing it is fatal. Extra binds (a tailnet
    // IP) often are not up yet at boot, before Tailscale connects, so they never abort the daemon: a failed
    // one is retried in the background until the address exists.
    if (!I.servers[0]->bind_to_port(I.bind_addrs[0], I.cfg.server.port)) {
        std::fprintf(stderr, "server: cannot bind %s:%d\n", I.bind_addrs[0].c_str(), I.cfg.server.port);
        for (auto & s : I.servers) s->stop();
        return false;
    }
    if (I.stopping) return true;
    std::vector<std::thread> threads;
    threads.emplace_back([&] { I.servers[0]->listen_after_bind(); });
    for (size_t i = 1; i < I.servers.size(); i++) {
        if (I.servers[i]->bind_to_port(I.bind_addrs[i], I.cfg.server.port)) {
            threads.emplace_back([&I, i] { I.servers[i]->listen_after_bind(); });
            continue;
        }
        std::fprintf(stderr, "server: cannot bind %s:%d yet, retrying every 3 s\n", I.bind_addrs[i].c_str(),
                     I.cfg.server.port);
        threads.emplace_back([&I, i] {
            while (!I.stopping.load()) {
                for (int k = 0; k < 30 && !I.stopping.load(); k++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (I.stopping.load()) return;
                // httplib marks a server decommissioned when a bind fails, and refuses every later bind;
                // stop() on a server that is not listening only clears that mark.
                I.servers[i]->stop();
                if (I.servers[i]->bind_to_port(I.bind_addrs[i], I.cfg.server.port)) {
                    std::fprintf(stderr, "server: bound %s:%d\n", I.bind_addrs[i].c_str(), I.cfg.server.port);
                    if (I.stopping.load()) I.servers[i]->stop();
                    I.servers[i]->listen_after_bind();
                    return;
                }
            }
        });
    }
    for (auto & t : threads) t.join();
    return true;
}

void Server::stop() {
    if (!impl_) return;
    impl_->stopping = true;
    impl_->hub.wake();
    for (auto & s : impl_->servers) s->stop();
}

}  // namespace judas
