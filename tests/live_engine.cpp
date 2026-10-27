// Live engine check on the real model (needs the GPU; stop any running daemon first).
// 1) one plain answer, 2) four concurrent agents with the same system prompt: later ones must reuse the
// prefix snapshot (cached_tokens > 0), 3) a tool call is parsed into tool_calls.
#include <chrono>
#include <cstdio>
#include <thread>

#include "core/config.h"
#include "engine/engine.h"
#include "test.h"

using namespace judas;

int main() {
    Config cfg = load_config(".");
    auto t0 = std::chrono::steady_clock::now();
    auto engine = Engine::create(cfg);
    std::printf("loaded %s in %.1f s\n", engine->model_name().c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());

    std::string sys = "You are a terse assistant. Answer in one short sentence.\n";
    for (int i = 0; i < 60; i++) sys += "Reference line " + std::to_string(i) + ": the quick brown fox jumps over the lazy dog.\n";

    ChatRequest r1;
    r1.messages = {{"system", sys}, {"user", "What is 2 + 2?"}};
    r1.enable_thinking = false;
    auto a = engine->chat(r1);
    std::printf("1) %s | finish %s prompt %d cached %d gen %d prefill %.2fs gen %.2fs\n", a.message.content.c_str(),
                a.finish_reason.c_str(), a.prompt_tokens, a.cached_tokens, a.completion_tokens, a.prefill_s, a.generate_s);
    CHECK(a.finish_reason == "stop");
    CHECK(a.message.content.find('4') != std::string::npos);

    const char * qs[4] = {"Name a primary color.", "What is the capital of France?", "How many legs does a spider have?",
                          "What gas do plants take in?"};
    ChatResult res[4];
    std::vector<std::thread> ts;
    t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; i++)
        ts.emplace_back([&, i] {
            ChatRequest r;
            r.messages = {{"system", sys}, {"user", qs[i]}};
            r.enable_thinking = false;
            r.tag = "live " + std::to_string(i);
            res[i] = engine->chat(r);
        });
    for (auto & t : ts) t.join();
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int reused = 0;
    for (int i = 0; i < 4; i++) {
        std::printf("2.%d) %s | prompt %d cached %d gen %d queue %.2fs\n", i, res[i].message.content.c_str(),
                    res[i].prompt_tokens, res[i].cached_tokens, res[i].completion_tokens, res[i].queue_s);
        CHECK(res[i].finish_reason == "stop");
        if (res[i].cached_tokens > 0) reused++;
    }
    std::printf("2) 4 agents in %.2f s, %d reused the shared prefix\n", wall, reused);
    CHECK(reused == 4);

    ChatRequest r3;
    r3.messages = {{"system", "You can call tools. Use them when asked."}, {"user", "Read the file /etc/hostname using the tool."}};
    r3.tools = {{"read_file", "Read a text file", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}, {"required", {"path"}}}}};
    auto c = engine->chat(r3);
    std::printf("3) finish %s, %zu tool call(s)", c.finish_reason.c_str(), c.message.tool_calls.size());
    if (!c.message.tool_calls.empty())
        std::printf(": %s %s", c.message.tool_calls[0].name.c_str(), c.message.tool_calls[0].arguments.c_str());
    std::printf("\n");
    CHECK(c.finish_reason == "tool_calls");
    CHECK(!c.message.tool_calls.empty() && c.message.tool_calls[0].name == "read_file");

    // 4) agent loop: the same conversation grows by an assistant turn and a tool result; the second call must
    // reuse almost everything (hybrid models need the prompt checkpoint for this).
    ChatRequest r4 = r3;
    r4.messages.push_back(c.message);
    r4.messages.back().role = "assistant";
    ChatMessage tool;
    tool.role = "tool";
    tool.tool_name = "read_file";
    tool.tool_call_id = c.message.tool_calls.empty() ? "x" : c.message.tool_calls[0].id;
    tool.content = "workstation";
    r4.messages.push_back(tool);
    auto d = engine->chat(r4);
    std::printf("4) follow-up: prompt %d cached %d (%.0f%%) prefill %.2fs: %s\n", d.prompt_tokens, d.cached_tokens,
                100.0 * d.cached_tokens / std::max(1, d.prompt_tokens), d.prefill_s, d.message.content.substr(0, 80).c_str());
    CHECK(d.cached_tokens >= c.prompt_tokens - 40);

    auto st = engine->state();
    std::printf("state: %d seqs, ctx_used %d, VRAM %ld/%ld MiB, %lld requests\n", st.max_agents, st.ctx_used,
                st.vram_used_mib, st.vram_total_mib, st.requests);
    TEST_MAIN_END();
}
