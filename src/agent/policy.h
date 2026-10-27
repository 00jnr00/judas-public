// Turn policy for agents, kept as pure functions so it can be unit tested without a model: how to read a reply
// (classify_reply), whether the turn goes on (should_continue), whether the model is stuck in a loop (LoopGuard),
// and whether the lead should be told to delegate (delegation_nudge). The agent loop in agent.cpp only applies
// the decisions made here.
#pragma once

#include <cstdint>
#include <deque>
#include <string>

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
};

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
// last `window` calls. A warning clears the evidence that triggered it; `pause_after` warnings in a short time
// mean the model ignored them, and the verdict becomes Pause. A write that succeeded resets the call evidence
// (the files changed, so repeating a read or a build is legitimate).
class LoopGuard {
public:
    enum class Verdict { Ok, Warn, Pause };
    struct Result {
        Verdict verdict = Verdict::Ok;
        std::string kind;      // "call" | "error"
        std::string tool;
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

// A tool result over max_chars keeps its head (2/3) and tail (1/3) with a note naming the recall id that
// returns the full text (the store keeps it). Unchanged when it fits or max_chars is 0.
std::string cap_tool_result(const std::string & out, size_t max_chars, const std::string & recall_id);

}  // namespace judas
