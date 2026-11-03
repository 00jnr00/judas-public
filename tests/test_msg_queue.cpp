// Messages sent while the lead is mid-turn: FIFO queue, cap, removal, edit, draining one turn per message,
// and what cancel does (drops the queue).
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include "agent/agent.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static std::string last_user(const ChatRequest & req) {
    for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it)
        if (it->role == "user") return it->content;
    return "";
}

class GateEngine : public Engine {
public:
    std::mutex mu;
    std::condition_variable cv;
    bool open = false;               // while false, a turn whose message contains HOLD waits
    std::vector<std::string> seen;   // user message of each lead turn, in order
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> &, const std::atomic<bool> * cancel) override {
        std::string u = last_user(req);
        std::unique_lock lk(mu);
        seen.push_back(u);
        if (u.find("HOLD") != std::string::npos)
            while (!open && !(cancel && cancel->load())) cv.wait_for(lk, std::chrono::milliseconds(20));
        ChatResult r;
        r.message.role = "assistant";
        r.message.content = "ok " + u;
        r.finish_reason = "stop";
        r.prompt_tokens = 10;
        r.completion_tokens = 5;
        return r;
    }
    void release() { { std::lock_guard lk(mu); open = true; } cv.notify_all(); }
    void close() { std::lock_guard lk(mu); open = false; }
    size_t turns() { std::lock_guard lk(mu); return seen.size(); }
    EngineState state() const override {
        EngineState s;
        s.model = "gate";
        s.loaded = true;
        s.max_agents = 4;
        s.ctx_total = 32000;
        return s;
    }
    std::string model_name() const override { return "gate"; }
    int count_tokens(const std::string & t) const override { return (int) t.size() / 4; }
};

template <class F> static bool wait_for(F f, int ms = 8000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

int main() {
    fs::path base = fs::path(JUDAS_TEST_TMP) / ("msg-queue-" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base / "wd");
    Config cfg;
    cfg.data_dir = (base / "data").string();
    cfg.agents.role_dirs = {};
    cfg.skills.paths = {(base / "skills").string()};
    cfg.skills.track_usage = false;
    cfg.memory.path = (base / "SHARED.md").string();
    cfg.mcp.enabled = false;
    cfg.learning.review = false;
    cfg.permissions.mode = "full-auto";
    cfg.permissions.allow_paths = {base.string()};
    GateEngine engine;
    Store store((base / "sessions.db").string());
    Journal journal(base / "snap");
    AgentManager agents(cfg, engine, store, journal);
    std::mutex ev_mu;
    std::vector<AgentEvent> events;
    agents.add_listener([&](const AgentEvent & e) { std::lock_guard lk(ev_mu); events.push_back(e); });
    std::string wd = (base / "wd").string();

    auto s = store.create_session("q", wd, "gate", "chat");
    // an idle session takes a message directly
    auto r0 = agents.submit_queued(s.id, "first HOLD", "", wd);
    CHECK(!r0.queued && r0.error.empty());
    REQUIRE(wait_for([&] { return engine.turns() == 1; }));

    // mid-turn: queued in order, with ids, visible in turn_info
    auto a = agents.submit_queued(s.id, "second", "", wd);
    auto b = agents.submit_queued(s.id, "third", "", wd);
    auto c = agents.submit_queued(s.id, "fourth", "\n[att]", wd);
    CHECK(a.queued && b.queued && c.queued);
    CHECK(a.id != b.id && b.id != c.id);
    auto q = agents.queued(s.id);
    REQUIRE(q.size() == 3);
    CHECK(q[0].text == "second" && q[1].text == "third" && q[2].text == "fourth");
    CHECK(agents.turn_info(s.id).queued.size() == 3);
    CHECK(engine.turns() == 1);   // nothing started early

    // remove and edit
    CHECK(agents.queue_remove(s.id, b.id));
    CHECK(!agents.queue_remove(s.id, b.id));
    CHECK(agents.queue_edit(s.id, a.id, "second edited"));
    CHECK(!agents.queue_edit(s.id, "nope", "x"));
    q = agents.queued(s.id);
    REQUIRE(q.size() == 2);
    CHECK(q[0].text == "second edited" && q[1].text == "fourth");

    // the turn ends: the rest drain in order, one turn each, and the session stays busy until the last
    engine.release();
    agents.wait_session(s.id);
    REQUIRE(wait_for([&] { return engine.turns() == 3; }));
    CHECK(agents.queued(s.id).empty());
    {
        std::lock_guard lk(engine.mu);
        CHECK(engine.seen[0] == "first HOLD");
        CHECK(engine.seen[1] == "second edited");
        CHECK(engine.seen[2] == "fourth\n[att]");
    }
    {
        std::lock_guard lk(ev_mu);
        int from_queue = 0, queue_events = 0;
        for (auto & e : events) {
            from_queue += e.type == "spawned" && e.data.value("from_queue", false);
            queue_events += e.type == "queue";
        }
        CHECK(from_queue == 2);
        CHECK(queue_events >= 5);
    }

    // the cap: 10 waiting, the 11th is refused with an error and nothing is lost
    engine.close();
    auto h = agents.submit_queued(s.id, "again HOLD", "", wd);
    CHECK(!h.queued);
    REQUIRE(wait_for([&] { return engine.turns() == 4; }));
    for (int i = 0; i < 10; i++) CHECK(agents.submit_queued(s.id, "m" + std::to_string(i), "", wd).queued);
    auto over = agents.submit_queued(s.id, "overflow", "", wd);
    CHECK(!over.queued && !over.error.empty());
    CHECK(agents.queued(s.id).size() == AgentManager::kMaxQueued);

    // cancel drops the whole queue (a stop means stop) and nothing from it runs afterwards
    agents.cancel(s.id);
    CHECK(agents.queued(s.id).empty());
    agents.wait_session(s.id);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(engine.turns() == 4);

    // after a cancel the session takes a fresh message as a normal turn
    engine.release();
    auto n = agents.submit_queued(s.id, "after cancel", "", wd);
    CHECK(!n.queued);
    agents.wait_session(s.id);
    CHECK(wait_for([&] { return engine.turns() == 5; }));
    fs::remove_all(base);
    return 0;
}
