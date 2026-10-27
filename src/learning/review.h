// Background review: after a lead turn, a hidden agent checks the conversation for durable memory or skill
// lessons. This header holds only the pure parts: the nudge decision table and the review prompts.
#pragma once

#include <string>

#include "core/config.h"

namespace judas {

// Per-session counters. tool_steps counts model steps with tools enabled since the last skill review; turns
// counts completed lead turns since the last memory review. in_flight blocks a second review for the session.
struct ReviewCounters {
    int tool_steps = 0;
    int turns = 0;
    bool in_flight = false;
};

// How the lead turn ended. Only Done may start a review.
enum class ReviewTurn { Done, Paused, Cancelled, Failed };

struct ReviewDecision {
    bool fire = false;
    bool memory = false;   // include the memory prompt
    bool skills = false;   // include the skill prompt
    ReviewCounters next;   // counters to store after this decision
};

// Decide whether this lead turn should start a background review. Thresholds are inclusive; 0 disables that
// nudge. When a review fires, only the nudge(s) it consumed are reset, so the other dimension can still fire
// on a later turn. next.in_flight is true when fire is true.
ReviewDecision decide(const ReviewCounters & counters, const LearningConfig & cfg, ReviewTurn outcome);

// The review prompt: combined when both flags are set, memory-only or skill-only otherwise. "" when neither
// flag is set.
std::string review_prompt(bool memory, bool skills);

}  // namespace judas
