// Hybrid compaction: pointer-drop big old tool results first, summarize the oldest turns only if still over.
#include "context/compactor.h"

#include <algorithm>
#include <cctype>
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

// "read_file {"path":"SPEC.md","offset":81}": the call that produced tool result `m`, found in the assistant
// message that issued it, so a digest says where its text came from rather than only its first line.
std::string call_label(const std::vector<ChatMessage> & v, size_t i) {
    const ChatMessage & m = v[i];
    if (m.tool_call_id.empty()) return m.tool_name;
    for (size_t j = i; j-- > 0;) {
        if (v[j].role != "assistant") continue;
        for (const auto & tc : v[j].tool_calls) {
            if (tc.id != m.tool_call_id) continue;
            std::string a = tc.arguments;
            for (char & c : a)
                if (c == '\n' || c == '\r') c = ' ';
            if (a.size() > 100) a = a.substr(0, 100) + "...";
            return tc.name + " " + a;
        }
    }
    return m.tool_name;
}

const char * const kRecallMark = "recall id=";
const char * const kSummaryHead = "[earlier conversation summarized]\n";
const int kBigToolTokens = 200;
const int kIndexMax = 24;  // recall index lines kept in a summary

// ---- rule pinning ----

const char * const kPinHead = "[pinned: project rules, kept verbatim through compaction]";
const char * const kPinEntry = "\n[pin] ";   // starts each file's section; the path follows to end of line

struct PinEntry {
    std::string path, text;
};

// Existing pinned message -> entries (so later passes keep what earlier ones pinned).
std::vector<PinEntry> parse_pin(const std::string & content) {
    std::vector<PinEntry> out;
    size_t at = content.find(kPinEntry);
    while (at != std::string::npos) {
        size_t line_end = content.find('\n', at + 1);
        if (line_end == std::string::npos) break;
        size_t next = content.find(kPinEntry, line_end);
        PinEntry e;
        e.path = content.substr(at + 7, line_end - at - 7);
        e.text = content.substr(line_end + 1, (next == std::string::npos ? content.size() : next) - line_end - 1);
        out.push_back(e);
        at = next;
    }
    return out;
}

bool ends_with_path(const std::string & full, const std::string & tail) {
    if (tail.empty() || tail.size() > full.size()) return false;
    if (full.compare(full.size() - tail.size(), tail.size(), tail) != 0) return false;
    return full.size() == tail.size() || full[full.size() - tail.size() - 1] == '/' || tail[0] == '/';
}

// File-looking words in the first user message ("follow SPEC.md", "see docs/rules.txt"), punctuation trimmed.
std::vector<std::string> named_paths(const std::string & text) {
    std::vector<std::string> out;
    std::string w;
    auto flush = [&] {
        while (!w.empty() && (w.back() == '.' || w.back() == ',' || w.back() == ':' || w.back() == ';')) w.pop_back();
        size_t dot = w.rfind('.');
        if (dot != std::string::npos && dot > 0 && dot + 1 < w.size() && w.size() - dot <= 9) out.push_back(w);
        w.clear();
    };
    for (char ch : text) {
        bool ok = std::isalnum((unsigned char) ch) || ch == '_' || ch == '.' || ch == '/' || ch == '-' || ch == '~';
        if (ok) w += ch;
        else flush();
    }
    flush();
    return out;
}

bool is_rule_file(const std::string & path, const std::vector<std::string> & named) {
    size_t slash = path.rfind('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    if (base == "AGENTS.md" || base == "CLAUDE.md") return true;
    if (base.size() > 3 && base.compare(base.size() - 3, 3, ".md") == 0 &&
        (path.rfind(".judas/", 0) == 0 || path.find("/.judas/") != std::string::npos))
        return true;
    for (const auto & n : named)
        if (ends_with_path(path, n) || ends_with_path(n, path)) return true;
    return false;
}

// Path of a read_file call that produced tool message `i`, with no offset (a partial read is not the rules).
std::string read_path(const std::vector<ChatMessage> & v, size_t i) {
    const ChatMessage & m = v[i];
    if (m.role != "tool" || m.tool_name != "read_file" || m.tool_call_id.empty()) return "";
    for (size_t j = i; j-- > 0;) {
        if (v[j].role != "assistant") continue;
        for (const auto & tc : v[j].tool_calls) {
            if (tc.id != m.tool_call_id) continue;
            try {
                json a = json::parse(tc.arguments);
                if (a.value("offset", 1L) > 1) return "";
                return a.value("path", std::string());
            } catch (...) {
                return "";
            }
        }
    }
    return "";
}

// First `limit` tokens of `s`, cut on a line boundary when one is near the end.
std::string head_tokens(const std::string & s, int limit, const TokenCounter & count) {
    if (count(s) <= limit) return s;
    size_t lo = 0, hi = s.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (count(s.substr(0, mid)) <= limit) lo = mid;
        else hi = mid;
    }
    size_t nl = s.rfind('\n', lo);
    if (nl != std::string::npos && nl > lo / 2) lo = nl;
    while (lo > 0 && ((unsigned char) s[lo] & 0xC0) == 0x80) lo--;
    return s.substr(0, lo) + "\n[truncated, pin limit]";
}

}  // namespace

bool is_pinned_message(const ChatMessage & m) {
    return m.role == "system" && m.content.rfind(kPinHead, 0) == 0;
}

static CompactionResult compact_impl(const std::vector<ChatMessage> & messages, const ContextConfig & cfg, int ctx_cap,
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
                std::string label = call_label(msgs, i);
                m.content = (label.empty() ? "" : "[" + label + "] ") + digest_line(m.content) +
                            " [full output recalled with recall id=" + ref + "]";
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
                // The summary drops the recall ids of the folded tool results; list them so the model can still
                // fetch any of them (the summarizer cannot be trusted to copy ids). Capped so it stays small.
                std::string index;
                int listed = 0, skipped = 0;
                for (size_t i = 0; i < run_end; i++) {
                    if (rest[i].role != "tool" || !recall_ref) continue;
                    std::string ref = recall_ref(rest[i]);
                    if (ref.empty()) continue;
                    if (listed >= kIndexMax) { skipped++; continue; }
                    std::string label = call_label(rest, i);
                    index += "- " + ref + ": " + (label.empty() ? digest_line(rest[i].content) : label) + "\n";
                    listed++;
                }
                if (skipped) index += "- (" + std::to_string(skipped) + " older outputs not listed)\n";
                folded.content = kSummaryHead + s;
                if (!index.empty()) folded.content += "\n\nRecall index (fetch any with recall id=...):\n" + index;
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

CompactionResult compact(const std::vector<ChatMessage> & messages, const ContextConfig & cfg, int ctx_cap,
                         const TokenCounter & count, const SpanSummarizer & summarize,
                         const std::function<std::string(const ChatMessage &)> & recall_ref) {
    if (cfg.pin_total <= 0 || cfg.pin_tokens <= 0 || messages.empty() || !count)
        return compact_impl(messages, cfg, ctx_cap, count, summarize, recall_ref);
    try {
        // Lift the old pinned message out so no pass can drop or summarize it; it goes back below.
        std::vector<PinEntry> pins;
        std::vector<ChatMessage> base;
        for (const auto & m : messages) {
            if (is_pinned_message(m)) {
                pins = parse_pin(m.content);
                continue;
            }
            base.push_back(m);
        }
        std::vector<std::string> named;
        for (const auto & m : base)
            if (m.role == "user") {
                named = named_paths(m.content);
                break;
            }
        // The pinned message counts toward the prompt: judge the trigger on the full history, then compact the rest.
        int pinned_before = total_tokens(messages, count) - total_tokens(base, count);
        CompactionResult res = compact_impl(base, cfg, std::max(1, ctx_cap - pinned_before), count, summarize, recall_ref);
        if (!res.changed) {
            CompactionResult same;
            same.messages = messages;
            same.before_tokens = same.after_tokens = total_tokens(messages, count);
            return same;
        }
        // A rule-file read that is no longer verbatim in the result was dropped or summarized: pin it.
        for (size_t i = 0; i < base.size(); i++) {
            std::string path = read_path(base, i);
            if (path.empty() || !is_rule_file(path, named)) continue;
            if (base[i].content.find(kRecallMark) != std::string::npos) continue;  // already a digest
            bool kept = false;
            for (auto & m : res.messages) {
                if (m.role != "tool" || m.tool_call_id != base[i].tool_call_id) continue;
                if (m.content == base[i].content) kept = true;
                else if (size_t at = m.content.find(kRecallMark); at != std::string::npos)  // say where the text went, not a second copy
                    m.content = "[read_file " + path + "] [kept verbatim in the pinned rules message; full output recalled with " +
                                m.content.substr(at, m.content.size() - at - (m.content.back() == ']' ? 1 : 0)) + "]";
            }
            if (kept) continue;
            std::erase_if(pins, [&](const PinEntry & e) { return e.path == path; });  // latest read wins
            pins.push_back({path, head_tokens(base[i].content, cfg.pin_tokens, count)});
        }
        if (!pins.empty()) {
            // Trim to pin_total, newest first: the oldest entries give way.
            int budget = cfg.pin_total;
            std::vector<PinEntry> keep;
            for (size_t k = pins.size(); k-- > 0;) {
                int t = count(pins[k].text);
                if (t > budget) pins[k].text = head_tokens(pins[k].text, std::max(0, budget), count);
                budget -= std::min(t, budget);
                keep.insert(keep.begin(), pins[k]);
                if (budget <= 0) break;
            }
            ChatMessage pm;
            pm.role = "system";
            pm.content = kPinHead;
            for (const auto & e : keep) pm.content += std::string(kPinEntry) + e.path + "\n" + e.text;
            size_t at = 0;
            while (at < res.messages.size() && res.messages[at].role != "user") at++;
            res.messages.insert(res.messages.begin() + std::min(at + 1, res.messages.size()), pm);
        }
        res.before_tokens = total_tokens(messages, count);
        res.after_tokens = total_tokens(res.messages, count);
        return res;
    } catch (...) {
        CompactionResult unchanged;
        unchanged.messages = messages;
        return unchanged;
    }
}

}  // namespace judas
