// Cowork work modes (design, research, write; engineering lives in agent.cpp): prompts, the per-session tools
// calc_record / source_add / doc_suggest, suggestion accept/reject, and the session file helpers behind
// GET/PUT /api/sessions/<id>/file. Contract: docs/COWORK-MODES.md.
#pragma once
#include <optional>

#include <string>

#include "core/json.h"
#include "snap/journal.h"
#include "store/store.h"
#include "tools/tools.h"

namespace judas::cowork {

// Every session mode the server, the TUI and the agent accept.
bool valid_mode(const std::string & m);
inline const char * kModeList = "chat, cowork, code, learn, swarm, engineering, design, research or write";

// Tool gating by session mode: whether `tool` is offered to an agent of a session in `mode` (lead = the session's lead
// agent, not a worker). Covers cad_* (engineering, design), calc_record (engineering, lead), source_add (research,
// lead) and doc_suggest (write, lead); every other tool is not decided here (true).
// A session keeps its work mode (engineering, design, research, write): its prompt, tools and state belong to it.
// The general modes (chat, cowork, code, learn, swarm) may still move between each other.
// Value of plain arithmetic after the last '=' (numbers, + - * / ^ **, parentheses, pi, sqrt); nullopt otherwise.
std::optional<double> eval_arith(const std::string & text);
bool mode_change_allowed(const std::string & from, const std::string & to);
bool tool_allowed(const std::string & tool, const std::string & mode, bool lead);

const char * design_prompt();
const char * research_prompt();
const char * write_prompt();

// Registers calc_record, source_add and doc_suggest. They only run in their own mode (ctx.mode, when set) and are
// listed to the lead only (the agent's tool gating).
void register_cowork_tools(ToolRegistry & reg, Store & store);

// Resolve `rel` inside `cwd`: "" + path in `out` when it is a relative path that stays inside cwd after symlinks
// are followed (the file need not exist unless must_exist), else the reason in the return value.
std::string resolve_in_cwd(const std::string & cwd, const std::string & rel, bool must_exist, std::string & out);

constexpr size_t kMaxFileBytes = 2u << 20;

struct FileResult {
    int status = 200;          // HTTP-style: 200, 400, 403, 404, 409, 413, 415
    std::string error;
    std::string path, text, hash;
};
// Text of a file inside cwd (hash = sha256 hex of the text). Refuses escapes, symlinks out, > 2 MB and binary files.
FileResult read_file(const std::string & cwd, const std::string & rel);
// Text documents in cwd for the Write file picker: relative paths of .md .txt .tex .rst .org .adoc .html files, newest
// first, at most `limit`, four folders deep, skipping hidden folders, node_modules, build output and symlinks.
std::vector<std::pair<std::string, long long>> list_documents(const std::string & cwd, size_t limit = 300);
// Save the user's own edit. 409 when the file's current hash differs from base_hash. A file that does not exist yet
// is created when base_hash is empty. Returns the new hash in `hash`.
FileResult write_file(const std::string & cwd, const std::string & rel, const std::string & text, const std::string & base_hash);

struct Outcome {
    int status = 200;
    json body;
};
// Accept a suggestion. spelling: replace `find` once (snapshotted through `journal` when given); stale -> 409.
// note: just marks it accepted. Only open suggestions can be accepted.
Outcome accept_suggestion(Store & store, Journal * journal, const std::string & session, const std::string & cwd,
                          const std::string & gid);
Outcome reject_suggestion(Store & store, const std::string & session, const std::string & gid);

json calc_json(const CalcRow & c);
json source_json(const SourceRow & s);
json suggestion_json(const SuggestionRow & g);

}  // namespace judas::cowork
