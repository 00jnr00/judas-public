// How an MCP server process is started: executable lookup, a scrubbed environment, and the bwrap sandbox.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"

namespace judas::mcp {

struct Launch {
    std::vector<std::string> argv;   // what to spawn (bwrap ... -- command, or the command itself)
    std::vector<std::string> env;    // exact environment, "K=V"
    std::string cwd;                 // for posix_spawn ("" when bwrap does the chdir)
    bool sandboxed = false;
    bool refused = false;            // fail closed: argv stays empty, the server is never spawned
    std::string note;                // why it is not sandboxed, or what the sandbox allows (for status output)
};

// The launch verdict for one server, a pure function so tests can cover every case. Fail closed: a server
// that should be sandboxed (sandbox = true, the default) but cannot be (bwrap missing, or
// permissions.sandbox is not "bwrap") is refused, unless permissions.allow_unsandboxed_mcp opts in;
// a server with sandbox = false opted out on its own. `reason` is empty when sandboxed, else one line.
struct SandboxVerdict {
    enum class What { sandboxed, unsandboxed, refuse };
    What what = What::refuse;
    std::string reason;
};
SandboxVerdict sandbox_verdict(const McpServerConfig & s, const PermissionsConfig & p, bool bwrap_present);

// Build the launch for one server using sandbox_verdict. A refused server keeps argv empty and logs the
// reason; inside the sandbox: root read-only, only `writable` paths writable, a fresh /tmp, secret
// directories hidden, network off unless the server sets network = true.
Launch build_launch(const Config & cfg, const McpServerConfig & s);

// "${VAR}" and a leading "~" expand; an unset variable becomes "".
std::string expand_value(const std::string & v);

// Tool-name glob (* and ?) match, used for tools_allow / tools_deny.
bool glob_match(const std::string & pattern, const std::string & text);

}  // namespace judas::mcp
