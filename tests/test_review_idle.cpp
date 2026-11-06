// Background reviews yield the model to user-facing agents: they queue until nothing user-facing runs, a user
// turn preempts a running one (it is requeued), a wall-clock cap stops a slow one, and review_max_steps bounds
// its model calls. Fake engine, no model.
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;
using namespace std::chrono_literals;

// Lead calls take `lead_ms` unless `lead_gate` is closed (then they wait for it to open). Review calls either
// finish at once, block until cancelled (review_block), or ask for a tool every step (review_tools).
class IdleEngine : public Engine {
public:
    std::atomic<bool> lead_gate{true};
    std::atomic<bool> review_block{false};
    std::atomic<bool> review_tools{false};
    std::atomic<int> lead_in_flight{0};
    std::atomic<int> overlap{0};          // a review call began while a lead call was in flight (or the reverse)
    std::atomic<int> review_calls{0};
    std::atomic<int> review_cancelled{0};
    std::atomic<int> review_calls_with_tools{0};

    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> &,
                    const std::atomic<bool> * cancel) override {
        ChatResult res;
        res.message.role = "assistant";
        res.finish_reason = "stop";
        res.prompt_tokens = 10;
        res.completion_tokens = 2;
        bool review = req.tag.rfind("review", 0) == 0;
        auto cancelled = [&] { return cancel && cancel->load(); };
        if (!review) {
            lead_in_flight++;
            while (!lead_gate.load() && !cancelled()) std::this_thread::sleep_for(5ms);
            std::this_thread::sleep_for(30ms);
            lead_in_flight--;
            res.message.content = "ok";
            return res;
        }
        if (lead_in_flight.load() > 0) overlap++;
        review_calls++;
        if (!req.tools.empty()) review_calls_with_tools++;
        if (review_block.load()) {
            while (!cancelled()) std::this_thread::sleep_for(5ms);
            review_cancelled++;
            res.finish_reason = "cancelled";
            return res;
        }
        if (review_tools.load() && !req.tools.empty()) {
            res.message.tool_calls.push_back({"c" + std::to_string(review_calls.load()), "memory_search",
                                              "{\"query\":\"q" + std::to_string(review_calls.load()) + "\"}"});
            res.finish_reason = "tool_calls";
            return res;
        }
        res.message.content = "nothing to save";
        return res;
    }
    EngineState state() const override { EngineState s; s.model = "idle"; s.loaded = true; s.max_agents = 4; s.ctx_total = 32000; return s; }
    std::string model_name() const override { return "idle"; }
    int count_tokens(const std::string & t) const override { return (int) t.size() / 4; }
};

template <class F>
static bool wait_for(F f, int ms = 8000) {
    for (int i = 0; i < ms / 10; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return f();
}

static size_t count(const json & j, const char * k) { return j[k].size(); }

struct Rig {
    fs::path base;
    Config cfg;
    IdleEngine engine;
    std::unique_ptr<Store> store;
    std::unique_ptr<Journal> journal;
    std::unique_ptr<AgentManager> agents;
    std::string wd;
    explicit Rig(const std::string & name, int wall_s = 300, int max_steps = 12) {
        base = fs::path(JUDAS_TEST_TMP) / (name + "-" + std::to_string(getpid()));
        fs::remove_all(base);
        fs::create_directories(base / "wd");
        cfg.data_dir = (base / "data").string();
        cfg.agents.role_dirs = {};
        cfg.skills.paths = {(base / "skills").string()};
        cfg.skills.track_usage = false;
        cfg.memory.path = (base / "SHARED.md").string();
        cfg.mcp.enabled = false;
        cfg.learning.review = true;
        cfg.learning.review_memory_turns = 1;   // every finished lead turn asks for a review
        cfg.learning.review_tool_steps = 0;
        cfg.learning.review_max_wall_s = wall_s;
        cfg.learning.review_max_steps = max_steps;
        cfg.permissions.mode = "full-auto";
        cfg.permissions.allow_paths = {base.string()};
        store = std::make_unique<Store>((base / "sessions.db").string());
        journal = std::make_unique<Journal>(base / "snap");
        agents = std::make_unique<AgentManager>(cfg, engine, *store, *journal);
        wd = (base / "wd").string();
    }
    std::string session() { return store->create_session("p", wd, "idle", "chat").id; }
    std::string review_status(const std::string & sid) {
        for (auto & r : agents->agents(sid))
            if (r.role == "review") return r.status;
        return "";
    }
};

// A review queued behind a running user turn does not start until the turn ends; a running review is
// preempted by a new user turn and requeued, and the model is never shared.
static void test_preempt_and_requeue() {
    Rig r("review-idle-preempt");
    std::string a = r.session(), b = r.session();
    r.engine.review_block = true;
    r.agents->submit_queued(a, "hello", "", r.wd);
    r.agents->wait_session(a);
    REQUIRE(wait_for([&] { return count(r.agents->review_state(), "running") == 1; }));

    // a user turn on ANOTHER session preempts the review (it used to cancel only its own session's review)
    r.engine.lead_gate = false;
    r.agents->submit_queued(b, "other session", "", r.wd);
    REQUIRE(wait_for([&] { return r.engine.review_cancelled.load() >= 1; }));
    REQUIRE(wait_for([&] { return count(r.agents->review_state(), "running") == 0; }));
    json st = r.agents->review_state();
    CHECK(count(st, "queued") == 1);          // requeued, not dropped
    CHECK(st["queued"][0]["requeues"] == 1);
    CHECK(st["queued"][0]["session"] == a);
    std::this_thread::sleep_for(150ms);       // the review must stay down while the user turn holds the model
    CHECK(count(r.agents->review_state(), "running") == 0);
    CHECK(r.engine.overlap.load() == 0);

    // the user turn ends: the old review restarts, and b's own review queues behind it
    r.engine.lead_gate = true;
    r.agents->wait_session(b);
    REQUIRE(wait_for([&] { return count(r.agents->review_state(), "running") == 1; }));
    st = r.agents->review_state();
    CHECK(st["running"][0]["session"] == a);
    CHECK(count(st, "queued") == 1);
    CHECK(st["queued"][0]["session"] == b);

    // let reviews finish: both run to completion, one at a time
    r.engine.review_block = false;
    REQUIRE(wait_for([&] {
        json s = r.agents->review_state();
        return count(s, "running") == 0 && count(s, "queued") == 0 && r.review_status(a) == "done" &&
               r.review_status(b) == "done";
    }));
    CHECK(r.engine.overlap.load() == 0);
}

// The wall-clock cap stops a review that never finishes, and frees the model.
static void test_wall_cap() {
    Rig r("review-idle-wall", 1);
    std::string a = r.session();
    r.engine.review_block = true;
    auto t0 = std::chrono::steady_clock::now();
    r.agents->submit_queued(a, "hello", "", r.wd);
    r.agents->wait_session(a);
    REQUIRE(wait_for([&] { return r.review_status(a) == "killed"; }, 8000));
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(s >= 0.9 && s < 6.0);
    json st = r.agents->review_state();
    CHECK(count(st, "running") == 0 && count(st, "queued") == 0);
    bool saw = false;
    for (auto & row : r.agents->agents(a))
        if (row.role == "review") saw = row.result.find("wall-clock cap") != std::string::npos;
    CHECK(saw);
}

// review_max_steps bounds model calls: the final no-tools call is the last of the N allowed.
static void test_max_steps() {
    Rig r("review-idle-steps", 300, 3);
    std::string a = r.session();
    r.engine.review_tools = true;
    r.agents->submit_queued(a, "hello", "", r.wd);
    r.agents->wait_session(a);
    REQUIRE(wait_for([&] { return r.review_status(a) == "done"; }));
    CHECK(r.engine.review_calls.load() == 3);
    CHECK(r.engine.review_calls_with_tools.load() == 2);
}

// With review off nothing is queued and the state is empty.
static void test_state_empty() {
    Rig r("review-idle-empty");
    r.cfg.learning.review_max_wall_s = 300;
    json st = r.agents->review_state();
    CHECK(count(st, "queued") == 0 && count(st, "running") == 0);
    CHECK(st["max_wall_s"] == 300);
}

int main() {
    test_preempt_and_requeue();
    test_wall_cap();
    test_max_steps();
    test_state_empty();
    TEST_MAIN_END();
}
