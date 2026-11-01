// Learn mode: a tutoring session grounded in the learner's own files. Pure helpers (file lists, digest, search,
// the progress record) live here so they can be tested; the agent wires the prompt and the two tools.
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

// The per-learner progress record, <data_dir>/learn/progress.json. Topics keyed by name.
// Applies one learn_progress call to `doc`; returns the reply text, *ok false on bad input.
std::string learn_progress_apply(json & doc, const json & args, bool * ok, const std::string & today);
std::string learn_progress_path(const Config & cfg);

// System prompt text for Learn sessions (the digest is appended by the caller).
const char * learn_prompt();
// Tools a Learn session may use; everything else (writes, shell, MCP) is withheld.
bool learn_tool_allowed(const std::string & name);

void register_learn_tools(ToolRegistry & reg);

}  // namespace judas
