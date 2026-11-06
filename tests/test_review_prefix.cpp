// The background review must advertise the same tool schemas as the lead, so the chat template renders an
// identical prompt head and the engine can reuse the lead's KV. Execution stays restricted (test_review).
#include <unistd.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

class RecEngine : public Engine {
public:
    std::mutex mu;
    struct Rec { std::string tag; std::vector<std::string> tools; };
    std::vector<Rec> recs;
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> &, const std::atomic<bool> *) override {
        Rec r{req.tag, {}};
        for (auto & t : req.tools) r.tools.push_back(t.name);
        { std::lock_guard lk(mu); recs.push_back(r); }
        ChatResult res;
        res.message.role = "assistant";
        res.message.content = "ok";
        res.finish_reason = "stop";
        res.prompt_tokens = 10;
        res.completion_tokens = 2;
        return res;
    }
    EngineState state() const override { EngineState s; s.model = "rec"; s.loaded = true; s.max_agents = 4; s.ctx_total = 32000; return s; }
    std::string model_name() const override { return "rec"; }
    int count_tokens(const std::string & t) const override { return (int) t.size() / 4; }
};

int main() {
    fs::path base = fs::path(JUDAS_TEST_TMP) / ("review-prefix-" + std::to_string(getpid()));
    fs::remove_all(base);
    fs::create_directories(base / "wd");
    Config cfg;
    cfg.data_dir = (base / "data").string();
    cfg.agents.role_dirs = {};
    cfg.skills.paths = {(base / "skills").string()};
    cfg.skills.track_usage = false;
    cfg.memory.path = (base / "SHARED.md").string();
    cfg.mcp.enabled = false;
    cfg.learning.review = true;
    cfg.learning.review_memory_turns = 1;   // the review fires after the first lead turn
    cfg.learning.review_tool_steps = 0;
    cfg.permissions.mode = "full-auto";
    cfg.permissions.allow_paths = {base.string()};
    RecEngine engine;
    Store store((base / "sessions.db").string());
    Journal journal(base / "snap");
    AgentManager agents(cfg, engine, store, journal);
    std::string wd = (base / "wd").string();
    auto s = store.create_session("p", wd, "rec", "chat");
    agents.submit_queued(s.id, "hello", "", wd);
    agents.wait_session(s.id);
    for (int i = 0; i < 400; i++) {
        { std::lock_guard lk(engine.mu); if (engine.recs.size() >= 2) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::lock_guard lk(engine.mu);
    REQUIRE(engine.recs.size() >= 2);
    const RecEngine::Rec * lead = nullptr, * review = nullptr;
    for (auto & r : engine.recs) {
        if (r.tag.rfind("lead", 0) == 0 && !lead) lead = &r;
        if (r.tag.rfind("review", 0) == 0 && !review) review = &r;
    }
    REQUIRE(lead && review);
    CHECK(!lead->tools.empty());
    CHECK(review->tools == lead->tools);
    TEST_MAIN_END();
}
