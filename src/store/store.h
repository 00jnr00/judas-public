// Sessions, agents and messages in SQLite (<data_dir>/sessions.db). WAL mode, one connection guarded by a mutex.
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

struct sqlite3;

namespace judas {

struct SessionRow {
    std::string id;            // "s-20261013-142233-ab12"
    std::string title;
    std::string cwd;
    std::string model;
    std::string mode = "chat"; // chat | cowork | code (the UI surface the session belongs to; unknown values are shown everywhere)
    int auto_resume = -1;      // -1 follows agents.auto_resume, 0 off, 1 on (per-session override)
    long long created_ms = 0, updated_ms = 0;
};

struct AgentRow {
    std::string id;            // "a-...."
    std::string session;
    std::string parent;        // "" for the session's lead agent
    std::string role;          // lead | worker | reviewer | <custom>
    std::string task;          // the prompt it was spawned with
    std::string status;        // queued | running | waiting | done | failed | killed
    std::string result;        // final report
    long long created_ms = 0, updated_ms = 0;
    long long prompt_tokens = 0, completion_tokens = 0;
};

struct MessageRow {
    long long id = 0;          // autoincrement
    std::string session, agent;
    std::string role;          // system | user | assistant | tool
    json body;                 // OpenAI-style message object (content, tool_calls, tool_call_id, reasoning_content)
    long long created_ms = 0;
};

// One full-text search result across sessions, ready for the session_search tool.
struct SearchHit {
    std::string session;       // session id
    std::string title;         // session title
    long long message_id = 0;
    std::string role;
    long long created_ms = 0;
    std::string snippet;       // matched excerpt, matches wrapped in [ ]
};

class Store {
public:
    explicit Store(const std::string & db_path);  // creates schema if missing
    ~Store();
    Store(const Store &) = delete;
    Store & operator=(const Store &) = delete;

    SessionRow create_session(const std::string & title, const std::string & cwd, const std::string & model,
                              const std::string & mode = "chat");
    std::optional<SessionRow> get_session(const std::string & id) const;
    // Newest updated first. before_ms > 0 returns only sessions updated strictly before it (the cursor for
    // the next page).
    std::vector<SessionRow> list_sessions(size_t limit = 50, long long before_ms = 0) const;
    void rename_session(const std::string & id, const std::string & title);
    bool set_session_mode(const std::string & id, const std::string & mode);  // false if no such session
    bool set_session_auto_resume(const std::string & id, int v);      // -1 inherit | 0 | 1; false if no such session
    void delete_session(const std::string & id);                      // cascades agents + messages

    AgentRow create_agent(const std::string & session, const std::string & parent, const std::string & role,
                          const std::string & task);
    void update_agent(const AgentRow & a);                            // status, result, token counts
    int interrupt_orphans();                                          // daemon start: 'running' rows from a dead process -> 'interrupted'
    std::optional<AgentRow> get_agent(const std::string & id) const;
    std::vector<AgentRow> list_agents(const std::string & session) const;

    long long add_message(const std::string & session, const std::string & agent, const std::string & role,
                          const json & body);
    std::vector<MessageRow> messages(const std::string & session, const std::string & agent) const;  // oldest first
    // One page of a session's history across ALL agents (agent_filter "" = every agent), oldest first.
    // Returns the newest `limit` messages with id < before_id (before_id 0 = from the end; limit 0 = all).
    // *more is set when older messages remain, *total to the session's message count.
    std::vector<MessageRow> messages_page(const std::string & session, long long before_id, size_t limit,
                                          bool * more = nullptr, long long * total = nullptr) const;
    long long last_message_id(const std::string & session) const;  // 0 if none

    // Full-text search over message content (SQLite FTS5). Returns matching messages, newest first.
    std::vector<MessageRow> search(const std::string & query, size_t limit = 50) const;

    // Cross-session search for the session_search tool: matching messages newest first, each with its
    // session title and a snippet (about 32 tokens, matches wrapped in [ ]). session_id "" means all
    // sessions. Query terms are quoted so FTS syntax in user text cannot throw.
    std::vector<SearchHit> search_messages(const std::string & query, int limit = 10,
                                           const std::string & session_id = "") const;

    // Scheduled tasks: a saved prompt that runs once at a time, on an interval, or on demand.
    struct TaskRow {
        std::string id, title, prompt, cwd, kind;  // kind: "manual" | "once" | "interval"
        long long run_at_ms = 0;    // kind "once": when to fire
        long long interval_s = 0;   // kind "interval": period
        long long next_ms = 0;      // next fire time (0 = not scheduled)
        bool enabled = true;
        std::string last_session;
        long long last_run_ms = 0, created_ms = 0;
    };
    TaskRow create_task(const TaskRow & t);          // fills id/created_ms/next_ms
    std::vector<TaskRow> list_tasks() const;         // newest first
    std::optional<TaskRow> get_task(const std::string & id) const;
    void update_task(const TaskRow & t);
    void delete_task(const std::string & id);
    std::vector<TaskRow> due_tasks(long long now) const;  // enabled, kind != manual, next_ms in (0, now]

    // Usage statistics for the Statistics screen:
    //   {turns, sessions, prompt_tokens, completion_tokens,
    //    daily:[{date:"yyyy-mm-dd", count}],   (one "turn" = one user message, local date)
    //    models:[{model, sessions}]}
    // daily covers at least the last `days` calendar days (local time), zero-filled on the client.
    json usage_stats(int days = 371) const;

    // Full text of a tool result by its tool_call_id (for the recall tool, after compaction pointer-drops it).
    std::optional<std::string> tool_output(const std::string & session, const std::string & tool_call_id) const;

private:
    void migrate();
    sqlite3 * db_ = nullptr;
    mutable std::mutex mu_;
};

std::string new_id(const char * prefix);  // prefix + "-" + yyyymmdd-hhmmss + "-" + 4 random hex
long long now_ms();

}  // namespace judas
