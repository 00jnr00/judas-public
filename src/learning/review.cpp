// Background review prompts and nudge decisions. The text is a trimmed port of the Hermes Agent review prompts (Nous Research, MIT; see THIRD_PARTY.md),
// limited to the tools Judas has: no support files, no curator adopt, no slash skill command.
#include "learning/review.h"

namespace judas {
namespace {

const char * MEMORY_PROMPT = R"(Review the conversation above and consider saving to memory if appropriate.

Focus on:
1. Has the user revealed things about themselves: their persona, desires, preferences, or personal details worth remembering?
2. Has the user expressed expectations about how you should behave, their work style, or ways they want you to operate?

If something stands out, save it with memory_append. Use memory_search or memory_read first if you need to see what is already there. If nothing is worth saving, say 'Nothing to save.' and stop.

You are a background reviewer with no user present. You may only call memory and skill tools (and session_search when it exists). Other tools are denied.)";

const char * SKILL_PROMPT = R"(Review the conversation above and update the skill library. Be ACTIVE: most sessions produce at least one skill update, even if small. A pass that does nothing is a missed learning opportunity, not a neutral outcome.

Target shape of the library: CLASS-LEVEL skills, each with a rich body. Not a long flat list of narrow one-session-one-skill entries. This shapes HOW you update, not WHETHER you update.

Signals to look for (any one of these warrants action):
- User corrected your style, tone, format, legibility, verbosity, or approach. Frustration signals like 'stop doing X', 'this is too verbose', 'don't format like this', 'why are you explaining', 'just give me the answer', 'you always do Y and I hate it', or an explicit 'remember this' are FIRST-CLASS skill signals, not just memory signals. Update the relevant skill(s) to embed the preference so the next session starts already knowing.
- User corrected your workflow, approach, or sequence of steps. Encode the correction as a pitfall or explicit step in the skill that governs that class of task.
- A non-trivial technique, fix, workaround, debugging path, or tool-usage pattern emerged that a future session would benefit from. Capture it.
- A skill that got loaded or consulted this session turned out to be wrong, missing a step, or outdated. Patch it NOW.

Preference order, prefer the earliest action that fits, but do pick one when a signal above fired:
1. UPDATE A CURRENTLY-LOADED SKILL. Look back through the conversation for skills the user loaded or you read with load_skill. If any of them covers the territory of the new learning, PATCH that one first. View it once with skill_manage action=view during this review, then patch it. It is the skill that was in play, so it is the right one to extend, but only if its source is agent.
2. UPDATE AN EXISTING UMBRELLA. Use skill_manage action=list and action=view to find a class-level skill that fits, then patch it.
3. CREATE A NEW CLASS-LEVEL UMBRELLA when no existing skill covers the class. The name MUST be at the class level. It MUST NOT be a specific PR number, error string, feature codename, library-alone name, or 'fix-X / debug-Y / audit-Z-today' session artifact. If the proposed name only makes sense for today's task, it is wrong: fall back to 1 or 2.

Read before you write: before patching or rewriting an existing skill, call skill_manage action=view for that skill during this review and base the change on what it returns. Content quoted earlier in the conversation does not count.

User-preference embedding (important): when the user expressed a style, format or workflow preference, the update belongs in the skill body, not just in memory. Memory captures who the user is and what the current situation and state of operations are; skills capture how to do this class of task for this user. When they complain about how you handled a task, the skill that governs that task needs to carry the lesson.

If you notice two existing skills that overlap, note it in your reply: the curator handles consolidation at scale.

Protected skills (DO NOT edit these):
- PINNED skills. You are an autonomous actor with no user present, so pin blocks your writes too, content updates included. Only the user, in a foreground session, can change a pinned skill.
- USER-OWNED skills: anything whose source is not agent (a hand-written local skill or an externally installed one). Your writes to these WILL be refused. This includes skills that were loaded or consulted this session: being in play does not make one yours to edit. If such a skill is wrong or outdated, say so in your reply; do not try to patch it.
If the only skills that need updating are protected, say 'Nothing to save.' and stop.

Do NOT capture (these become persistent self-imposed constraints that bite you later when the environment changes):
- Environment-dependent failures: missing binaries, fresh-install errors, post-migration path mismatches, 'command not found', unconfigured credentials, uninstalled packages. The user can fix these: they are not durable rules.
- Negative claims about tools or features ('browser tools do not work', 'X tool is broken', 'cannot use Y from execute_code'). These harden into refusals the agent cites against itself for months after the actual problem was fixed.
- Session-specific transient errors that resolved before the conversation ended. If retrying worked, the lesson is the retry pattern, not the original failure.
- One-off task narratives. A user asking 'summarize today's market' or 'analyze this PR' is not a class of work that warrants a skill.
- Unresolved failures: if the session ended WITHOUT actually finding a working method, you tried several things, none worked, and told the user to check manually, do NOT write those attempts up as a 'reliable workflow' or 'recommended approach'. That presents an untested sequence of failures as validated guidance a future session will trust and repeat. Either say 'Nothing to save', or, only if you are independently confident of a real working alternative (not something you are merely guessing might work), capture ONLY that alternative: never the dead ends, and never dressed up as best practice.

If a tool failed because of setup state, capture the FIX (install command, config step, env var to set) under an existing setup or troubleshooting skill, never 'this tool does not work' as a standalone constraint.

'Nothing to save.' is a real option but should NOT be the default. If the session ran smoothly with no corrections and produced no new technique, just say 'Nothing to save.' and stop. Otherwise, act.

You are a background reviewer with no user present. You may only call memory and skill tools (and session_search when it exists). Other tools are denied.)";

const char * COMBINED_PROMPT = R"(Review the conversation above and update two things:

**Memory**: who the user is. Did the user reveal persona, desires, preferences, personal details, or expectations about how you should behave? Save facts about the user and durable preferences with memory_append. Use memory_search or memory_read first if you need to see what is already there.

**Skills**: how to do this class of task. Be ACTIVE: most sessions produce at least one skill update. A pass that does nothing is a missed learning opportunity, not a neutral outcome.

Target shape of the skill library: CLASS-LEVEL skills with a rich body. Not a long flat list of narrow one-session-one-skill entries.

Signals that warrant a skill update (any one is enough):
- User corrected your style, tone, format, legibility, verbosity, or approach. Frustration is a FIRST-CLASS skill signal, not just a memory signal. 'stop doing X', 'don't format like this', 'I hate when you Y': embed the lesson in the skill that governs that task so the next session starts fixed.
- A non-trivial technique, fix, workaround, or debugging path emerged.
- A skill that was loaded or consulted turned out wrong, missing, or outdated: patch it now.

Preference order for skills, pick the earliest that fits:
1. UPDATE A CURRENTLY-LOADED SKILL. Check what skills were loaded with load_skill in the conversation. If one covers the learning, PATCH it first. View it once with skill_manage action=view during this review. It was in play; it is the right place, provided its source is agent.
2. UPDATE AN EXISTING UMBRELLA. Use skill_manage action=list and action=view to find the right one, then patch it.
3. CREATE A NEW CLASS-LEVEL UMBRELLA when nothing exists. Name it at the class level: NOT a PR number, error string, codename, library-alone name, or 'fix-X / debug-Y' session artifact. If the name only fits today's task, fall back to 1 or 2.

Read before you write: before patching or rewriting an existing skill, call skill_manage action=view for that skill during this review and base the change on what it returns. Content quoted earlier in the transcript does not count.

User-preference embedding: when the user complains about how you handled a task, update the skill that governs that task; memory alone is not enough. Memory says who the user is and what the current situation and state of operations are; skills say how to do this class of task for this user. Both should carry user-preference lessons when relevant.

If you notice overlapping existing skills, mention it: the curator handles consolidation.

Protected skills (DO NOT edit these):
- PINNED skills. Pin blocks autonomous writes entirely, content updates included, because no user is present to consent. Only a foreground session can change one.
- USER-OWNED skills: anything whose source is not agent (hand-written or externally installed). Your writes to these WILL be refused, including to skills loaded or consulted this session. If one is wrong, say so in your reply instead.
If the only skills that need updating are protected, say 'Nothing to save.' and stop.

Do NOT capture as skills (these become persistent self-imposed constraints that bite you later when the environment changes):
- Environment-dependent failures: missing binaries, fresh-install errors, post-migration path mismatches, 'command not found', unconfigured credentials, uninstalled packages. The user can fix these: they are not durable rules.
- Negative claims about tools or features ('browser tools do not work', 'X tool is broken', 'cannot use Y from execute_code'). These harden into refusals the agent cites against itself for months after the actual problem was fixed.
- Session-specific transient errors that resolved before the conversation ended. If retrying worked, the lesson is the retry pattern, not the original failure.
- One-off task narratives. A user asking 'summarize today's market' or 'analyze this PR' is not a class of work that warrants a skill.
- Unresolved failures: if the session ended WITHOUT actually finding a working method, you tried several things, none worked, and told the user to check manually, do NOT write those attempts up as a 'reliable workflow' or 'recommended approach'. That presents an untested sequence of failures as validated guidance a future session will trust and repeat. Either say 'Nothing to save', or, only if you are independently confident of a real working alternative (not something you are merely guessing might work), capture ONLY that alternative: never the dead ends, and never dressed up as best practice.

If a tool failed because of setup state, capture the FIX (install command, config step, env var to set) under an existing setup or troubleshooting skill, never 'this tool does not work' as a standalone constraint.

Act on whichever of the two dimensions has real signal. If genuinely nothing stands out on either, say 'Nothing to save.' and stop, but do not reach for that conclusion as a default.

You are a background reviewer with no user present. You may only call memory and skill tools (and session_search when it exists). Other tools are denied.)";

}  // namespace

ReviewDecision decide(const ReviewCounters & counters, const LearningConfig & cfg, ReviewTurn outcome) {
    ReviewDecision d;
    d.next = counters;
    if (!cfg.review || counters.in_flight || outcome != ReviewTurn::Done) return d;

    bool memory = cfg.review_memory_turns > 0 && counters.turns >= cfg.review_memory_turns;
    bool skills = cfg.review_tool_steps > 0 && counters.tool_steps >= cfg.review_tool_steps;
    if (!memory && !skills) return d;

    d.fire = true;
    d.memory = memory;
    d.skills = skills;
    if (memory) d.next.turns = 0;
    if (skills) d.next.tool_steps = 0;
    d.next.in_flight = true;
    return d;
}

std::string review_prompt(bool memory, bool skills) {
    if (memory && skills) return COMBINED_PROMPT;
    if (memory) return MEMORY_PROMPT;
    if (skills) return SKILL_PROMPT;
    return "";
}

}  // namespace judas
