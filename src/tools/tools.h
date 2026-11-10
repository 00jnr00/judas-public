// Tools the model can call. Every tool that changes files goes through the Journal first, so it can be reverted.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "snap/journal.h"

namespace judas {

struct ToolContext {
    std::string cwd;                 // agent working directory (relative paths resolve here)
    const Config * cfg = nullptr;
    Journal * journal = nullptr;     // may be null in tests that do not write
    std::string checkpoint;          // current step's checkpoint id (set by the agent loop before each call)
    std::string session, agent;
    // The session's mode (chat, cowork, engineering, design, research, write ...), "" in tests. Write mode refuses edits to
    // existing files; the cowork tools refuse to run outside their own mode.
    std::string mode;
    // A background actor with no user present (the review agent). Tools that can touch user-owned state use
    // this to refuse writes no user consented to.
    bool autonomous = false;
    // Asks the user to approve an action (permission mode "suggest", or irreversible actions in "auto-edit").
    // Returns true if approved. Null = deny everything that needs approval.
    std::function<bool(const std::string & what)> approve;
    // Like approve, but the permission mode never answers for the user: full-auto still asks. For edits to
    // user-owned state (the day planner). Null = deny (no user present, or no approver).
    std::function<bool(const std::string & what)> approve_strict;
    // Judas Health Link. health_tainted: this session has read health data (set by the agent loop from the store);
    // the registry then refuses every tool that sends content off the machine (see health_taint_refusal) and the
    // shell runs with no network. taint_session marks the session (the health tool calls it when it returns data).
    bool health_tainted = false;
    bool no_network = false;
    std::function<void()> taint_session;
    // Location: the session has called location_get (the agent loop sets it from the store). The registry then refuses
    // memory_append and skill_manage, so a position is never saved to memory or a skill; note_location marks the session.
    bool location_used = false;
    std::function<void()> note_location;
    // Long tools report progress here (the agent loop turns it into a "swarm" UI event). May be null.
    std::function<void(const json &)> progress;
    // Explicit-ask tools (Tool::grant): the latest message the user typed in this session, and whether it grants
    // the tool being called. Set by the agent loop for the session lead only; empty/false everywhere else.
    std::string grant_text;
    bool granted = false;
};

struct ToolResult {
    bool ok = true;
    std::string output;              // what the model sees (truncated by the tool to a sane size)
    bool irreversible = false;       // a side effect a revert cannot undo happened
};

struct Tool {
    std::string name;
    std::string description;         // one or two lines, shown to the model
    json parameters;                 // JSON schema object
    bool writes = false;             // changes files (journaled)
    bool may_be_irreversible = false;  // shell, process control, network writes
    std::string source;              // "" = built in; "mcp:<server>" = from an MCP server (see src/mcp)
    // Explicit-ask tool: set = the tool is offered to (and runs for) the session lead only in a turn whose latest
    // user-typed message makes this return true. Never offered to workers, reviews, probes, Learn or health-tainted
    // sessions. The model cannot grant it to itself: the text is what the user typed, not tool output.
    std::function<bool(const std::string & user_text)> grant;
    std::function<ToolResult(const json & args, ToolContext & ctx)> run;
};

class ToolRegistry {
public:
    // Thread-safe: MCP servers add and remove tools while agents run. A replaced or removed Tool stays alive
    // (parked in retired_), so a pointer an agent took from list()/find() never dangles.
    void add(Tool t);                                       // replaces a tool with the same name
    bool remove(const std::string & name);
    const Tool * find(const std::string & name) const;
    std::vector<const Tool *> list() const;                 // registration order
    // Subset by name; unknown names are ignored. Empty `names` = all.
    std::vector<const Tool *> select(const std::vector<std::string> & names) const;
    // Run with error capture: unknown tool, bad args, exceptions -> ok=false with a message the model can act on.
    ToolResult call(const std::string & name, const json & args, ToolContext & ctx) const;

private:
    mutable std::mutex mu_;
    std::vector<std::unique_ptr<Tool>> tools_;
    std::vector<std::unique_ptr<Tool>> retired_;
};

// "" if `tool` may run in a health-tainted session, else the refusal text. `source` is Tool::source ("mcp:<server>"
// for MCP tools). Refused: memory_append, skill_manage, web_search, web_fetch, every MCP tool. show_image, notify and
// session_search have narrower rules inside the tools themselves.
std::string health_taint_refusal(const std::string & tool, const std::string & source);
extern const char * const kHealthTaintMessage;
// "" if `tool` may run in a session that has used location_get, else the refusal text.
std::string location_taint_refusal(const std::string & tool);

// read_file, write_file, edit_file, list_dir, glob, grep, shell, web_search, web_fetch,
// snapshot_list, snapshot_diff, snapshot_revert
void register_builtin_tools(ToolRegistry & reg);

// view_image, screenshot, show_image (they return images through [[image:ID|name]] markers; see src/attach).
void register_attach_tools(ToolRegistry & reg);

// Read-side path policy: resolves like read_file and applies the secret/deny lists. Returns "" if allowed.
std::string check_read_path(const ToolContext & ctx, const std::string & path, std::string & resolved);

// check_read_path plus the allow_paths / deny_paths / working-directory containment (for tools that copy a file
// somewhere the user will see it). Returns "" if allowed, else the reason.
std::string check_allowed_read_path(const ToolContext & ctx, const std::string & path, std::string & resolved);

// Path policy shared by every writing tool: resolves relative to ctx.cwd, expands ~, and checks
// permissions.allow_paths / deny_paths. Returns "" if allowed, else the reason.
std::string check_write_path(const ToolContext & ctx, const std::string & path, std::string & resolved);

// The shell tool's hard floor, usable by any tool that takes a command or a path (the MCP client uses them).
// Each returns "" if allowed, else the refusal text for the model. They hold in every permission mode.
std::string safety_floor_command(const std::string & cmd);               // sudo/su/credential commands, auth db writes
std::string safety_floor_path(const ToolContext & ctx, const std::string & path);  // configured secret paths and secret files
// Absolute path of an executable bwrap on PATH (or /usr/bin/bwrap), "" when bubblewrap is not installed.
std::string find_bwrap();
// Keep the head and tail of long output (the policy every tool uses): "... N chars truncated ...".
std::string truncate_output(const std::string & s, size_t max);

}  // namespace judas
