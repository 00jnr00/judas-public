// Agents: a prompt + its own conversation + a tool set, all sharing the one Engine. The lead agent of a
// session can spawn workers (spawn/send/wait/report tools); each agent runs on its own thread and the
// engine batches them. Feedback loop: lead plans, workers act and report, a reviewer checks, lead decides.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "engine/engine.h"
#include "snap/journal.h"
#include "store/store.h"
#include "tools/tools.h"

namespace judas {

struct Role {
    std::string name;                  // lead | worker | reviewer | custom
    std::string description;
    std::string prompt;                // role system prompt (Markdown body)
    std::vector<std::string> tools;    // allowed tool names; empty = all
    int priority = 1;
    int max_steps = -1;                // < 0 = agents.max_steps
};

// Built-in roles plus *.md files (YAML-ish frontmatter: name, description, tools, priority, max_steps) in role_dirs.
std::map<std::string, Role> load_roles(const Config & cfg);

// Everything the UI needs to draw the agent board, pushed as it happens.
struct AgentEvent {
    std::string type;                  // spawned | delta | reasoning | tool_call | tool_result | message | status | done
    std::string session, agent, parent, role;
    json data;                         // type-specific payload
};
using AgentListener = std::function<void(const AgentEvent &)>;

class AgentManager {
public:
    AgentManager(const Config & cfg, Engine & engine, Store & store, Journal & journal);
    ~AgentManager();

    // Start (or continue) a session with a user message for its lead agent. Returns immediately with the
    // session id; progress arrives through listeners. cwd = working directory for tools.
    std::string submit(const std::string & session_id, const std::string & user_message, const std::string & cwd);
    // Block until the session's lead agent finishes its current turn; returns its final answer.
    std::string wait_session(const std::string & session_id);
    void cancel(const std::string & session_id);

    // Where the session's lead turn stands. status: idle | running | paused | done | error | killed.
    // A paused turn hit a cap (auto-continues, steps, or a loop) with the job unfinished; reason says which.
    struct TurnInfo {
        std::string status = "idle", reason;
        int open_items = 0;       // unticked items in the lead's update_plan checklist
        std::string plan;         // the checklist text
    };
    TurnInfo turn_info(const std::string & session_id) const;
    // Continue a paused turn: the lead gets the plan as context and a short "carry on" message. Returns false if
    // the session has no paused lead (nothing is started then).
    bool resume(const std::string & session_id, const std::string & cwd = "");

    int add_listener(AgentListener fn);   // returns handle
    void remove_listener(int handle);
    // Approval hook for permission prompts (CLI asks on the terminal, web asks in the page). Default denies.
    void set_approver(std::function<bool(const std::string & session, const std::string & what)> fn);

    std::vector<AgentRow> agents(const std::string & session) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace judas
