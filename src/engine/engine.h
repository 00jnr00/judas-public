// The engine: one model loaded in-process (libllama), shared by up to engine.max_agents sequences.
// Any number of threads may call chat() at once; one scheduler thread owns the llama_context and puts
// every active request's next token (plus at most one prefill chunk) into the same llama_decode batch.
// Shared prompt prefixes are reused: a request first reuses its own sequence's cached tokens, else a RAM
// snapshot of a matching prefix (system prompt), so spawning an agent does not re-read the system prompt.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"

namespace judas {

struct ChatMessage {
    std::string role;                  // system | user | assistant | tool
    std::string content;
    std::string reasoning;             // assistant thinking (kept for templates that want it)
    struct ToolCall {
        std::string id, name, arguments;  // arguments = JSON text
    };
    std::vector<ToolCall> tool_calls;  // assistant only
    std::string tool_call_id;          // tool only
    std::string tool_name;             // tool only
};

struct ToolSpec {
    std::string name, description;
    json parameters;                   // JSON schema
};

struct Sampling {
    float temp = -1, top_p = -1, min_p = -1;   // < 0 = model default from config
    int top_k = -1;
    int max_tokens = -1;                        // < 0 = engine.max_tokens
    unsigned seed = 0xFFFFFFFF;                 // default = random
};

struct ChatRequest {
    std::vector<ChatMessage> messages;
    std::vector<ToolSpec> tools;
    Sampling sampling;
    bool enable_thinking = true;
    int priority = 1;                  // 0 = foreground (user is watching), 1 = normal, 2 = background
    std::string tag;                   // who is asking, for /state ("agent a-..", "v1 <client>")
};

struct ChatDelta {                     // streamed while generating (parsed incrementally)
    std::string content;               // new visible text
    std::string reasoning;             // new thinking text
};

struct ChatResult {
    ChatMessage message;               // final parsed assistant message (content, reasoning, tool_calls)
    std::string finish_reason;         // stop | length | tool_calls | cancelled | error
    std::string error;
    int prompt_tokens = 0, cached_tokens = 0, completion_tokens = 0;
    double prefill_s = 0, generate_s = 0, queue_s = 0;
};

struct EngineState {
    std::string model, model_path;
    bool loaded = false, hybrid = false;
    int max_agents = 0, active = 0, queued = 0;
    int ctx_total = 0, ctx_used = 0;
    long vram_used_mib = -1, vram_total_mib = -1;
    double gen_tok_s = 0;              // aggregate over the last few seconds
    struct Seq {
        int id;
        std::string tag, phase;        // idle | prefill | generate
        int tokens;
        int priority;
    };
    std::vector<Seq> seqs;
    // KV admission (unified pool). ctx_used is what is physically cached (idle caches included, they are
    // evictable); ctx_active is what live requests hold; ctx_free is what a new request could still claim
    // after reserving generation room for every live request. Requests that do not fit wait in the queue.
    int ctx_active = 0, ctx_free = 0, kv_reserve = 0;
    double prefill_duty = 1.0;         // engine.prefill_duty: prefills allowed per busy decode step
    int prefill_chunk_busy = 0;        // engine.busy_prefill_chunk(): chunk used while others generate
    int prefill_max_wait_ms = 0;       // engine.prefill_max_wait_ms: foreground starvation guard
    long long preemptions = 0;         // requests pushed back to the queue because the pool ran out
    struct Queued {
        std::string tag;
        int priority;
        int prompt_tokens;
        double waited_s;
        int preempted;                 // times this request was already preempted
    };
    std::vector<Queued> queue;
    long long total_prompt_tokens = 0, total_completion_tokens = 0, requests = 0;
};

class Engine {
public:
    virtual ~Engine() = default;
    // Loads cfg.active_model(). Throws std::runtime_error if the model cannot load.
    static std::unique_ptr<Engine> create(const Config & cfg);

    // Blocking. on_delta may return false to cancel. Thread-safe; concurrent calls are batched together.
    virtual ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on_delta = {},
                            const std::atomic<bool> * cancel = nullptr) = 0;
    virtual EngineState state() const = 0;
    virtual std::string model_name() const = 0;
    // Token count of a text with the loaded vocab (for prompt-size and budgets).
    virtual int count_tokens(const std::string & text) const = 0;
};

// OpenAI chat-completions JSON <-> engine types (used by the /v1 server and tests).
std::vector<ChatMessage> messages_from_oai(const json & messages);
std::vector<ToolSpec> tools_from_oai(const json & tools);
json message_to_oai(const ChatMessage & m);

}  // namespace judas
