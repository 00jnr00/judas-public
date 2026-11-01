// CPU-only check of the Strata backend's tokenizer path (vocab-only model from a synthetic GGUF; no GPU, no engine).
// Expected ids come from Strata's own reference tokenizer (tools/strata_tokenizer.py). Skips when the GGUF is absent.
#include <cstdlib>
#include <vector>
#include <filesystem>

#include "core/config.h"
#include "engine/engine.h"
#include <memory>
#include "test.h"

namespace judas { int strata_tokenizer_probe(const std::string & gguf, const std::string & text, std::vector<int> & out); }
using namespace judas;

int main() {
    const char * home = std::getenv("HOME");
    std::string gguf = std::string(home ? home : "") + "/models/strata/IQ2_XS/Qwen3.8-Flash-Next-GSQ-RCO-IQ2_XS-00001-of-00002.gguf";
    if (!std::filesystem::exists(gguf)) {
        std::puts("skip: Strata GGUF not present");
        return 0;
    }
    // Engine::create would start the engine; the tokenizer path alone is exercised through a dry config.

    struct Case { const char * text; std::vector<int> ids; };
    const Case cases[] = {
        {"Hello, world!", {9419, 11, 1814, 0}},
        {"<|im_start|>user\nhi<|im_end|>\n", {248045, 846, 198, 5834, 248046, 198}},
        {"na\xC3\xAFve caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E x_j", {3267, 36328, 571, 50203, 220, 247359, 830, 5206}},
        {"  two  spaces\n\n", {220, 1330, 220, 12258, 271}},
    };
    for (const auto & c : cases) {
        std::vector<int> got;
        REQUIRE(strata_tokenizer_probe(gguf, c.text, got) == 0);
        CHECK(got == c.ids);
    }

    // ---- the whole backend against a fake engine process (same protocol as `strata --serve`, canned reply)
    {
        std::string pack = std::string(home) + "/models/strata/packs/iq2_xs";
        if (std::filesystem::exists(pack + "/tokenizer/chat_template.jinja")) {
            Config cfg;
            ModelConfig m;
            m.backend = "strata";
            m.path = gguf;
            setenv("JUDAS_STRATA_FAKE_ENGINE", (std::string(JUDAS_TESTS_DIR) + "/fixtures/fake_strata.py").c_str(), 1);
            m.strata_pack = pack;
            m.strata_mtp = "/x";
            m.ctx_total = 4096;
            cfg.models["t"] = m;
            cfg.engine.model = "t";
            cfg.data_dir = std::string(JUDAS_TEST_TMP) + "/strata-data";
            auto eng = Engine::create(cfg);
            ChatRequest req;
            req.messages = {{"system", "You are terse."}, {"user", "Say hello."}};
            req.enable_thinking = false;
            std::string streamed;
            auto res = eng->chat(req, [&](const ChatDelta & d) { streamed += d.content; return true; });
            CHECK(res.error.empty());
            CHECK(res.finish_reason == "stop");
            CHECK(res.message.content == "Hello, world!");
            CHECK(streamed == "Hello, world!");
            CHECK(res.completion_tokens == 4);
            CHECK(res.cached_tokens > 0 && res.cached_tokens < res.prompt_tokens);
            CHECK(res.prompt_tokens > 10);
            auto st = eng->state();
            CHECK(st.loaded && st.max_agents == 1 && st.requests == 1);
            // a second request on the same engine, cancelled before it starts
            std::atomic<bool> cancel{true};
            auto r2 = eng->chat(req, {}, &cancel);
            CHECK(r2.finish_reason == "cancelled");
            eng.reset();   // QUIT, reap the child
        }
    }
    TEST_MAIN_END();
}
