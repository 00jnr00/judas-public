// Turn policy for agents, kept as pure functions so it can be unit tested without a model: how to read a reply
// (classify_reply), whether the turn goes on (should_continue), whether the model is stuck in a loop (LoopGuard),
// and whether the lead should be told to delegate (delegation_nudge). The agent loop in agent.cpp only applies
// the decisions made here.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "core/json.h"

namespace judas {

enum class ReplyKind {
    ToolCalls,     // the reply carries tool calls: run them
    Final,         // plain text that reads like an answer
    Empty,         // nothing visible (usually thinking only)
    Announce,      // announces the next step instead of making it: "Let me view the file:" or "I'll fix it."
    TextToolCall,  // a tool call written as text that the parser did not pick up
    Truncated,     // finish_reason "length": the reply hit the output (or context) limit
};

const char * reply_kind_name(ReplyKind k);

// finish_reason is the engine's ("stop" | "tool_calls" | "length" | ...). Truncated wins over everything: a cut
// off tool call must never run, even if the parser produced a partial one.
ReplyKind classify_reply(const std::string & content, bool has_tool_calls, const std::string & finish_reason);

// Open and ticked items in an update_plan checklist ("- [ ] item", "- [x] item", numbered or bulleted).
struct PlanState {
    int open = 0, done = 0;
};
PlanState plan_state(const std::string & plan);

struct TurnState {
    int auto_continues = 0;   // harness nudges used this turn
    int truncations = 0;      // replies cut off at the token limit this turn
    int empty_streak = 0;     // consecutive empty replies; at kEmptyWrapUpAfter the model is told to wrap up
    bool wrapping = false;    // a wrap-up instruction was given: a plain reply now ends the turn
};

// Two empty (or no-progress) replies in a row switch from "keep going" to "wrap up": say what is done and what is
// left, then stop. A third empty reply after the instruction pauses the turn.
constexpr int kEmptyWrapUpAfter = 2;
std::string wrap_up_message(const std::string & why);

// Per-turn wall-clock budget (agents.turn_wall_s, 0 = off).
inline bool wall_budget_exceeded(double elapsed_s, int limit_s) { return limit_s > 0 && elapsed_s >= limit_s; }

enum class Action { Execute, Continue, Finish, Pause };

struct Decision {
    Action action = Action::Finish;
    std::string nudge;    // Continue: the message to put to the model
    std::string reason;   // Pause (and Continue): why, for the status the server reports
};

// What to do after one model reply. Without a tool call the turn does not end unless the reply reads like a
// final answer AND the plan has no open items; otherwise the model gets a short nudge, up to max_auto_continues
// per turn, after which the turn pauses (a scheduler can resume it). completion_tokens is for the truncation text.
Decision should_continue(ReplyKind kind, const PlanState & plan, TurnState & st, int max_auto_continues,
                         int completion_tokens);

// The text of the truncation nudge: names the limit and tells the model how to split a big write.
std::string truncation_nudge(bool was_tool_call, int completion_tokens);

// Normalize a tool error so the same compile error compares equal across runs: digits (line numbers, timings)
// and whitespace runs are dropped, long output is cut.
std::string normalize_error(const std::string & output);

// Detects a model repeating itself: the same tool call (name + args), or the same tool error, K times within the
// last `window` calls; two calls taking turns (A B A B A) count as one loop. A warning clears the evidence that triggered it; `pause_after` warnings in a short time
// mean the model ignored them, and the verdict becomes Pause. A write that succeeded resets the call evidence
// (the files changed, so repeating a read or a build is legitimate).
class LoopGuard {
public:
    enum class Verdict { Ok, Warn, Pause };
    struct Result {
        Verdict verdict = Verdict::Ok;
        std::string kind;      // "call" | "error" | "cycle" (two calls alternating)
        std::string tool;
        std::string other;     // cycle: the second tool of the pair
        int count = 0;
    };
    LoopGuard(int repeats = 3, int pause_after = 2, int window = 16, int calm_after = 25)
        : repeats_(repeats), pause_after_(pause_after), window_(window), calm_after_(calm_after) {}
    // track_call = false for tools whose repetition is normal (done, update_plan, wait_agents, ...).
    Result observe(const std::string & tool, const std::string & args, bool ok, bool wrote, const std::string & output,
                   bool track_call = true);
    int warnings() const { return warnings_; }

private:
    struct Entry {
        uint64_t call = 0, err = 0;
        std::string tool;
    };
    std::deque<Entry> hist_;
    int repeats_, pause_after_, window_, calm_after_;
    int warnings_ = 0, since_warn_ = 0;
};

std::string loop_nudge(const LoopGuard::Result & r, bool can_spawn);
// The pause reason when a warning was ignored ("looping: ...").
std::string loop_pause_reason(const LoopGuard::Result & r);

// A tool call the model wrote into its reply text instead of making a real one. Two formats are read:
//   <tool_call><function=NAME><parameter=KEY>VALUE</parameter>...</function></tool_call>
//   {"name": NAME, "arguments": {...}}   (also "parameters", a JSON list of them, or a ```json fence)
struct TextCall {
    std::string name;
    json args = json::object();
};
enum class TextCallParse { None, Ok, Malformed };   // None: no tool call text; Malformed: it looks like one but is not usable
// Only names in `known` are accepted. A JSON reply must be nothing but the call(s), so an answer that merely quotes an
// example is never run. Parameter values are strings; coerce_text_args fixes the types from the tool's schema.
TextCallParse parse_text_tool_calls(const std::string & content, const std::vector<std::string> & known,
                                    std::vector<TextCall> & out);
// Same, plus each known tool's parameter names. With them, a call that drops the <function=NAME> line entirely
// ("<tool_call> <parameter=metric>..</parameter> </function>") is read as the one tool whose parameters cover
// every parameter in the call; it stays Malformed when no tool or more than one tool fits.
TextCallParse parse_text_tool_calls(const std::string & content, const std::vector<std::string> & known,
                                    const std::map<std::string, std::vector<std::string>> & params,
                                    std::vector<TextCall> & out);
void coerce_text_args(json & args, const json & schema);
std::string text_call_retry_nudge();

// The lead keeps editing module code itself while the plan has several open items: remind it to delegate.
struct DelegationState {
    int consecutive_edits = 0;
};
// Call after each tool call. is_edit = a file-writing tool; is_delegation = spawn_agent / wait_agents / send_agent.
// Returns true when the reminder should be injected (the counter restarts, so it repeats every `threshold` edits).
bool delegation_nudge(DelegationState & st, bool is_edit, bool is_delegation, int open_items, int threshold,
                      bool can_spawn);

// Read streak: consecutive read-only tool calls without a write or a build/test. Local models tend to keep reading
// instead of starting. At `nudge` reads the agent is told to write; at `force` further reads are refused until a
// write/edit or a build/test resets the streak. Calls that are neither (spawn_agent, update_plan, done, ...) leave
// the streak alone.
enum class CallKind { Read, Reset, Neutral };
struct ReadStreak {
    int count = 0;
};
enum class StreakEvent { None, Nudge, Force };
// Classify one tool call. `command` is the shell command (ignored for other tools); `writes` = the tool edits files.
CallKind classify_call(const std::string & tool, const std::string & command, bool writes);
// True when a Read call must be refused now (force > 0 and the streak has reached it).
bool read_streak_blocked(const ReadStreak & st, int force);
// Record a call. Returns Nudge / Force on the call that reaches the threshold (0 disables a threshold).
StreakEvent read_streak_observe(ReadStreak & st, CallKind kind, int nudge, int force);
std::string read_streak_message(StreakEvent ev, int count);

// ---- Swarm lead delivery ----------------------------------------------------------------------------------
// A Swarm lead has to deliver: once its workers are done it writes the deliverable from their results first and
// verifies afterwards. The pieces below are pure so the rules are unit tested with fake tool results.

// After the workers finished (or after repeated empty turns) verification tools are refused until the lead
// writes a file or answers. `pending` is set by the agent loop and cleared by a successful write.
struct DeliverState {
    bool pending = false;
};
// True when the tool call is verification or scraping that must wait: web_fetch, web_search, and a shell command
// that runs curl, wget or sleep. Reading and writing files and the other tools stay available.
bool deliver_blocks_call(const std::string & tool, const std::string & command);
// True when the call writes the deliverable: write_file with at least 400 characters. An edit or a small scratch
// file does not lift the gate.
bool deliver_satisfied(const std::string & tool, size_t content_chars);
std::string deliver_blocked_message();
// The harness message that hands the lead its workers' reports. `reports` is the reports JSON text.
std::string workers_done_message(const std::string & reports);
// Nudge after the lead produced empty turns in a row.
std::string report_now_message();

// Consecutive empty lead turns (no text, no tool call). observe() returns true when `limit` is reached (0 = off)
// and restarts the count.
struct EmptyStreak {
    int count = 0;
};
bool empty_streak_observe(EmptyStreak & st, bool empty, int limit);

// ---- Per-URL retry cap ----------------------------------------------------------------------------------
// A fetch failed when the tool errored, or it "succeeded" with a rate limit stub (local_rate_limited, 429, too
// many requests) or an empty JS shell (almost no text, or an enable-JavaScript notice).
bool fetch_stub_text(const std::string & output);   // a rate limit stub or a JavaScript-required notice
bool fetch_result_failed(bool tool_ok, const std::string & output);
// http(s) URLs a shell command fetches with curl or wget (empty when the command runs neither).
std::vector<std::string> fetch_urls_in_command(const std::string & command);
// Counts failures per URL (fragment, trailing slash and scheme/host case ignored). After `cap` failures the URL is
// refused with a give-up message. cap 0 = off. Not thread safe: one per agent.
class UrlGuard {
public:
    explicit UrlGuard(int cap = 2) : cap_(cap) {}
    void set_cap(int cap) { cap_ = cap; }
    // "" when the fetch may go ahead, else the give-up text for the model.
    std::string blocked(const std::string & url) const;
    void record(const std::string & url, bool failed);   // a success clears the count
    int failures(const std::string & url) const;
private:
    int cap_;
    std::map<std::string, int> fails_;
};
std::string normalize_url(const std::string & url);

// A tool result over max_chars keeps its head (2/3) and tail (1/3) with a note naming the recall id that
// returns the full text (the store keeps it). Unchanged when it fits or max_chars is 0.
std::string cap_tool_result(const std::string & out, size_t max_chars, const std::string & recall_id);

}  // namespace judas
