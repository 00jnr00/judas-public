// Coach (P0): propose-only review of a past session. A hidden, low-priority model call reads the session
// transcript (trimmed to fit the context) and proposes a harness variant: a prompt overlay, skills, and
// tool-guidance notes, each with a one-line rationale that cites transcript steps. The variant is written as
// plain files under <data_dir>/coach/<timestamp>-<session>/ with status "proposed"; a human adopts or rejects
// it. Nothing is re-executed and nothing changes until adoption. Edits that target permissions, the sandbox or
// the safety floor are refused in code (coach_forbidden), both when proposed and again when adopted or read.
// Inspired by Harness^2 (arXiv 2610.14224); clean-room implementation, see docs/COACH.md.
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "engine/engine.h"
#include "store/store.h"

namespace judas {

struct CoachEdit {
    std::string kind;        // overlay | skill | tool
    std::string name;        // skill name or tool name; empty for overlay
    std::string text;        // overlay rules, skill body, or tool note
    std::string rationale;   // one line citing steps, e.g. "[12-15] read the same file four times"
    std::string description; // skill only: one line
    std::vector<std::string> triggers;   // skill only
};

struct CoachProposal {
    std::string summary;
    std::vector<CoachEdit> edits;
};

struct CoachDiscard {
    std::string what;        // "skill foo", "overlay", ...
    std::string reason;
};

// Parse the model's reply (format in docs/COACH.md). Never throws; unusable blocks are dropped. *err is set
// only when nothing at all could be read.
CoachProposal coach_parse(const std::string & reply, std::string * err = nullptr);

// True when the text or name tries to reach permissions, the sandbox, privilege escalation, secrets paths,
// approvals, or the instruction hierarchy. *why names the match.
bool coach_forbidden(const std::string & text, std::string * why = nullptr);

// Check one edit against the safety floor, name rules, rationale rule and the size caps in cfg.coach.
// Returns "" when acceptable, else the reason it is refused.
std::string coach_check_edit(const Config & cfg, const CoachEdit & e);

// Transcript rendering. Steps are numbered from 1 in stored order (the numbers rationales cite).
// label = the agent's role when it is not the lead; secrets = expanded paths whose mention withholds the step's text.
std::string coach_render_step(const MessageRow & m, int number, size_t output_cap, const std::string & label = "",
                              const std::vector<std::string> & secrets = {});
// System messages and hidden review agents are skipped; the others are numbered in order.
std::vector<std::string> coach_render_steps(const std::vector<MessageRow> & msgs, size_t output_cap,
                                            const std::map<std::string, std::string> & agent_roles = {},
                                            const std::vector<std::string> & secrets = {});
// Join steps within max_chars. Keeps the first step (the task) and as many of the latest steps as fit, with a
// single elision marker for the omitted middle; original step numbers are kept.
std::string coach_trim_steps(const std::vector<std::string> & steps, size_t max_chars);
// Full transcript for a session at the given size budget (shrinks tool outputs first, then drops the middle).
std::string coach_transcript(const Config & cfg, const Store & store, const std::string & session, size_t max_chars);

std::string coach_system_prompt(const Config & cfg);
std::string coach_user_prompt(const SessionRow & s, const std::string & transcript, const std::string & signals = "");
std::string coach_signals(const Store & store, const std::string & sid);  // harness-measured facts per agent

// The same facts as numbers, summed over a session's agents (review agents excluded); the longest read-only
// streak is the maximum over agents. status is the lead agent's row status.
struct CoachStats {
    std::string status;
    int tool_calls = 0, writes = 0, longest_read_streak = 0, nudges = 0;
    long long tokens_in = 0, tokens_out = 0;
};
CoachStats coach_stats(const Store & store, const std::string & sid);

// ---- storage ---------------------------------------------------------------------------------------------

struct CoachVariant {
    std::string id;          // directory name: YYYYMMDD-HHMMSS-<session>
    std::string session;
    std::string status;      // proposed | adopted | rejected
    std::string model;
    std::string summary;
    long long created_ms = 0;
    std::string dir;
    std::vector<CoachEdit> edits;
    std::vector<CoachDiscard> discarded;
    bool probed = false;           // a probe has been run (see coach/probe.h)
    std::string probe_verdict;     // mechanical one-line verdict of the latest probe
};

std::string coach_root(const Config & cfg);           // <data_dir>/coach
std::string coach_overlay_path(const Config & cfg);   // adopted overlay file
bool coach_valid_id(const std::string & id);          // no path tricks

// Write a variant from a proposal (edits that fail coach_check_edit are listed as discarded). Returns it.
CoachVariant coach_write_variant(const Config & cfg, const std::string & session, const CoachProposal & p,
                                 const std::string & model, long long now_ms);
std::vector<CoachVariant> coach_list(const Config & cfg);      // newest first
bool coach_load(const Config & cfg, const std::string & id, CoachVariant & out);

struct CoachAdoptResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> skills_added;
    std::vector<std::string> skipped;   // "name: reason"
    bool overlay_appended = false;
};
// Adopt a proposed variant: copy its skills into the skills dir (never over an existing skill) and append the
// overlay and tool notes to the overlay file. Re-checks every file against the safety floor first.
CoachAdoptResult coach_adopt(const Config & cfg, const std::string & id);
// Mark a proposed variant rejected. False (with *err) when missing or not proposed.
bool coach_reject(const Config & cfg, const std::string & id, std::string * err = nullptr);

// The adopted overlay as a prompt block: "" when coach is off or the file is empty. Sections that match the
// safety floor are dropped, and the block is capped.
std::string coach_overlay_block(const Config & cfg);

// ---- running ---------------------------------------------------------------------------------------------

struct CoachRunResult {
    bool ok = false;
    std::string error;
    CoachVariant variant;
};
// Read the session ("last" = most recently updated), call the loaded model once at background priority, parse,
// write the variant. Blocks. Does not re-execute anything.
CoachRunResult coach_propose(const Config & cfg, Engine & engine, Store & store, const std::string & session,
                             const std::atomic<bool> * cancel = nullptr);

// Record that a probe ran (meta.json "probed": true plus a verdict line). False when the proposal is missing.
bool coach_set_probe(const Config & cfg, const std::string & id, const std::string & verdict);

json coach_variant_json(const CoachVariant & v, bool full);

}  // namespace judas
