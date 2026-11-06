// Learn mode: a tutoring session grounded in the learner's own files. Pure helpers (file lists, digest, search,
// the old progress path) live here so they can be tested; the agent wires the prompt and the Learn tools.
// context_files are read only: nothing in this module opens one for writing.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "tools/tools.h"

namespace judas {

// Expand cfg.learn.context_files (~ and per-segment * ? [] globs) into existing regular files, sorted and
// de-duplicated. A directory entry stands for its direct children (files only).
std::vector<std::string> learn_context_paths(const Config & cfg);
// True when `path` (resolved or not) is one of the context files; used to refuse writes to them.
bool learn_is_context_file(const Config & cfg, const std::string & path);

// Compact digest: each file's head, the budget (cfg.learn.max_context_tokens * 4 bytes) split evenly.
// "" when there are no files or the budget is 0.
std::string learn_context_digest(const Config & cfg);
// Lines containing `query` (case-insensitive) as "name:line: text", at most `max`.
std::vector<std::string> learn_context_search(const Config & cfg, const std::string & query, size_t max = 30);
// One context file by base name or full path (only listed files), capped at max_bytes. "" + *err on failure.
std::string learn_context_read(const Config & cfg, const std::string & which, size_t max_bytes, std::string * err);

// Old per-learner record, <data_dir>/learn/progress.json. Retired record: the learn_progress tool is gone, learn/progress.json is only read, once, into the topics store.
std::string learn_progress_path(const Config & cfg);

// Validates one learn_ask call. Returns "" when fine, else a message the model can act on. *norm (if given) gets the
// cleaned arguments: kind, prompt, options (strings, at most 8), want_confidence, want_thinking.
std::string learn_ask_check(const json & args, json * norm = nullptr);
// True for a tool whose successful call ends the lead's turn (the learner must answer first).
bool learn_ends_turn(const std::string & tool);

// System prompt text for Learn sessions (the digest is appended by the caller).
const char * learn_prompt();
// Tools a Learn session may use; everything else (writes, shell, MCP) is withheld.
bool learn_tool_allowed(const std::string & name);

void register_learn_tools(ToolRegistry & reg);

}  // namespace judas
