// Auto-resume policy: when a paused lead turn may be resumed without asking. Pure functions, no state, so the
// rules are unit-tested; the server keeps the counters and calls these.
#pragma once

#include <string>

namespace judas {

constexpr int kAutoResumeStallLimit = 3;   // resumes in a row with no progress before auto-resume gives up

struct AutoResumeInput {
    bool enabled = false;          // effective setting: the session override, else agents.auto_resume
    std::string status;            // turn status; only "paused" is ever resumed (never error or killed)
    long long paused_for_ms = 0;   // how long the turn has been paused
    int delay_s = 30;              // agents.auto_resume_delay_s
    int max_per_day = 20;          // agents.auto_resume_max
    int attempts_today = 0;
    int stalled = 0;               // consecutive auto-resumes that made no progress
};

enum class AutoResumeAction { None, Wait, Resume };   // None = do not resume (now or later); Wait = not yet

inline AutoResumeAction decide_auto_resume(const AutoResumeInput & in) {
    if (!in.enabled || in.status != "paused") return AutoResumeAction::None;
    if (in.attempts_today >= in.max_per_day) return AutoResumeAction::None;
    if (in.stalled >= kAutoResumeStallLimit) return AutoResumeAction::None;
    if (in.paused_for_ms < (long long) in.delay_s * 1000) return AutoResumeAction::Wait;
    return AutoResumeAction::Resume;
}

// Did a resumed stretch get anywhere: a file-changing tool call, or a plan item ticked off.
inline bool resume_made_progress(long long writes_before, long long writes_after, int open_before, int open_after) {
    return writes_after > writes_before || open_after < open_before;
}

}  // namespace judas
