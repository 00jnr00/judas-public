// Hybrid compaction: pointer-drop big old tool results first, summarize the oldest turns only if still over.
#include "context/compactor.h"

#include <algorithm>
#include <cstddef>

namespace judas {

namespace {

int msg_tokens(const ChatMessage & m, const TokenCounter & count) {
    int n = count(m.content) + count(m.reasoning);
    for (const auto & tc : m.tool_calls) n += count(tc.arguments);
    return n;
}

int total_tokens(const std::vector<ChatMessage> & v, const TokenCounter & count) {
    int n = 0;
    for (const auto & m : v) n += msg_tokens(m, count);
    return n;
}

// First line of the text, capped at ~120 chars.
std::string digest_line(const std::string & s) {
    std::string line = s.substr(0, s.find('\n'));
    if (line.size() > 120) line = line.substr(0, 120) + "...";
    return line;
}

const char * const kRecallMark = "recall id=";
const char * const kSummaryHead = "[earlier conversation summarized]\n";
const int kBigToolTokens = 200;

}  // namespace

CompactionResult compact(const std::vector<ChatMessage> & messages, const ContextConfig & cfg, int ctx_cap,
                         const TokenCounter & count, const SpanSummarizer & summarize,
                         const std::function<std::string(const ChatMessage &)> & recall_ref) {
    CompactionResult res;
    res.messages = messages;
    try {
        if (messages.empty() || !count) return res;
        res.before_tokens = res.after_tokens = total_tokens(messages, count);
        if (res.before_tokens <= ctx_cap * cfg.trigger_pct) return res;
        const double target = ctx_cap * cfg.target_pct;

        // Turns: a user message plus everything up to the next user message.
        std::vector<size_t> users;
        for (size_t i = 0; i < messages.size(); i++)
            if (messages[i].role == "user") users.push_back(i);
        if (users.empty()) return res;
        const size_t head_end = users[0] + 1;  // system prompt (if any) .. first user message
        // Protect the recent tail by a TOKEN budget, not a message count: a coding task is one user turn with a
        // long tool chain, and the model may read several big files in a single step, so a fixed message count
        // could shield many large outputs and leave the prompt over the cap. Walk back from the end, keeping
        // whole messages until the protected tail would exceed ~half the target; everything older (losslessly
        // recoverable via recall) is compactable. Always keep at least the final message.
        const double tail_budget = target * 0.5;
        size_t tail_start = messages.size();
        int tail_tokens = 0;
        while (tail_start > head_end) {
            int t = count(messages[tail_start - 1].content) + count(messages[tail_start - 1].reasoning);
            if (tail_start < messages.size() && tail_tokens + t > tail_budget) break;
            tail_tokens += t;
            tail_start--;
        }
        if (tail_start <= head_end) return res;  // nothing compactable

        std::vector<ChatMessage> & msgs = res.messages;
        int total = res.before_tokens;

        // Stage 1: pointer-drop large tool results in the span, oldest first.
        if (recall_ref) {
            for (size_t i = head_end; i < tail_start && total > target; i++) {
                ChatMessage & m = msgs[i];
                if (m.role != "tool") continue;
                if (m.content.find(kRecallMark) != std::string::npos) continue;
                int before = count(m.content);
                if (before <= kBigToolTokens) continue;
                std::string ref = recall_ref(m);
                if (ref.empty()) continue;
                m.content = digest_line(m.content) + " [full output recalled with recall id=" + ref + "]";
                total += count(m.content) - before;
                res.dropped_tool_results++;
                res.recalled++;
                res.changed = true;
            }
        }

        // Stage 2: fold the oldest whole turns into one summary message.
        if (total > target && summarize) {
            // Turn boundaries inside the span: segment starts are user messages (the first segment has none).
            std::vector<size_t> bounds;
            for (size_t i = head_end + 1; i < tail_start; i++)
                if (msgs[i].role == "user") bounds.push_back(i);
            bounds.push_back(tail_start);
            size_t b = 0;
            int turns = 0;
            std::vector<ChatMessage> head(msgs.begin(), msgs.begin() + head_end);
            std::vector<ChatMessage> rest(msgs.begin() + head_end, msgs.begin() + tail_start);
            std::vector<ChatMessage> tail(msgs.begin() + tail_start, msgs.end());
            // `rest` is consumed from the front; `folded` is the summary message once made.
            bool have_summary = false;
            ChatMessage folded;
            folded.role = "system";
            size_t run_end = 0;  // exclusive end of the folded run within `rest`
            while (b < bounds.size()) {
                size_t next_end = bounds[b++] - head_end;
                // Grow the run until the estimate (summary cost ignored) fits, or the span is exhausted.
                int run_cost = 0;
                for (size_t i = run_end; i < next_end; i++) run_cost += msg_tokens(rest[i], count);
                run_end = next_end;
                turns++;
                total -= run_cost;
                if (total > target && b < bounds.size()) continue;
                // Summarize the whole folded run once, then re-measure with the real summary cost.
                std::vector<ChatMessage> run(rest.begin(), rest.begin() + run_end);
                std::string s = summarize(run);
                folded.content = kSummaryHead + s;
                have_summary = true;
                int with = total + count(folded.content);
                if (with <= target || b >= bounds.size()) { total = with; break; }
                // Still over with the summary: keep growing (total excludes the summary cost for the run).
            }
            if (have_summary) {
                std::vector<ChatMessage> out = head;
                out.push_back(folded);
                out.insert(out.end(), rest.begin() + run_end, rest.end());
                out.insert(out.end(), tail.begin(), tail.end());
                msgs = std::move(out);
                res.summarized_turns = turns;
                res.changed = true;
            }
        }

        res.after_tokens = total_tokens(msgs, count);
        if (!res.changed) res.messages = messages;
        return res;
    } catch (...) {
        CompactionResult unchanged;
        unchanged.messages = messages;
        return unchanged;
    }
}

}  // namespace judas
