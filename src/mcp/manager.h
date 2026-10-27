// McpManager: starts the configured MCP servers, mirrors their tools into the ToolRegistry as ordinary tools,
// and keeps them in step (list_changed, restarts). See docs/MCP.md.
//
// Every MCP call runs through the same agent-loop path as a built-in tool (pre-tool/post-tool hooks, loop guard,
// journal checkpoint, permission mode). Inside the tool, before anything reaches the server: the SAFETY FLOOR
// (command and path checks on the arguments), schema validation (in ToolRegistry::call), and approval for tools
// that may change things.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "mcp/client.h"
#include "tools/tools.h"

namespace judas::mcp {

struct ServerStatus {
    std::string name;
    std::string state;        // starting | ready | failed | disabled
    std::string error;
    std::string protocol;     // negotiated version
    std::string server_name;  // from serverInfo
    bool sandboxed = false;
    std::string sandbox_note;
    int tools_registered = 0;
    int tools_filtered = 0;   // dropped by tools_allow / tools_deny
    int restarts = 0;
};

class Manager {
public:
    Manager(const Config & cfg, ToolRegistry & reg);
    ~Manager();
    Manager(const Manager &) = delete;
    Manager & operator=(const Manager &) = delete;

    void start();                         // connect every enabled server in the background; returns at once
    bool wait_ready(int timeout_ms);      // true once no server is still "starting"
    std::vector<ServerStatus> status() const;

private:
    struct Server;
    void connect(Server * s);
    void refresh(Server * s);
    ToolResult run_tool(Server * s, const std::string & tool, bool readonly, const json & args, ToolContext & ctx);

    const Config & cfg_;
    ToolRegistry & reg_;
    std::vector<std::unique_ptr<Server>> servers_;
    std::atomic<bool> stopping_{false};
};

// Convert a tools/call result (content blocks) to text for the model: text as is, resources as text or a note,
// images and audio saved under files_dir with a short note and the path. Truncates to max_chars (head and tail).
// ok = !isError. Exposed for tests.
ToolResult convert_result(const json & result, const std::string & files_dir, const std::string & tool, size_t max_chars);

// First sentence or paragraph of a description, at most `max` characters, cut at a word boundary.
std::string trim_description(const std::string & d, size_t max);

}  // namespace judas::mcp
