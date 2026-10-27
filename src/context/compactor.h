// Compaction: shrink an agent's message history when it nears the context cap, losing as little as possible.
// Strategy (mode "compact"): keep the first user message and the last keep_turns turns verbatim; in the older
// span, drop or condense tool results, replacing a big one with a one-line digest plus a recall id so the full
// text (still in the store) can be fetched with the recall tool; only if still over target, summarize the
// oldest remaining span with the model. Pure logic: the caller supplies token counting and a summarizer.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/config.h"
#include "engine/engine.h"  // ChatMessage

namespace judas {

struct CompactionResult {
    std::vector<ChatMessage> messages;   // the new, shorter history
    int before_tokens = 0, after_tokens = 0;
    int dropped_tool_results = 0, recalled = 0, summarized_turns = 0;
    bool changed = false;
};

// Count tokens of one message's text (caller wraps the engine tokenizer).
using TokenCounter = std::function<int(const std::string &)>;
// Summarize a span of messages into one short assistant-visible note (caller wraps the engine).
using SpanSummarizer = std::function<std::string(const std::vector<ChatMessage> &)>;

// Decide and apply compaction. If the history already fits under target, returns it unchanged (changed=false).
// `recall_ref(msg)` returns a stable id for a tool message whose body was stored (so the digest can cite it);
// empty means that message's full text is not recoverable, so it is summarized rather than pointer-dropped.
CompactionResult compact(const std::vector<ChatMessage> & messages, const ContextConfig & cfg, int ctx_cap,
                         const TokenCounter & count, const SpanSummarizer & summarize,
                         const std::function<std::string(const ChatMessage &)> & recall_ref);

}  // namespace judas
