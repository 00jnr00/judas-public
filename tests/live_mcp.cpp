// Manual smoke test of the MCP client against a real server. Not run by ctest.
//   live_mcp [--no-sandbox] [--network] [--writable PATH]... <tool> <args-json> <server command...>
// Starts the server (under bwrap unless --no-sandbox) through the same Manager the daemon uses, lists the
// registered tools, calls <tool> (its registered, prefixed name) and prints the output length and head.
#include <cstdio>
#include <cstring>
#include <string>

#include "mcp/manager.h"

using namespace judas;

int main(int argc, char ** argv) {
    Config cfg;
    cfg.permissions.mode = "full-auto";
    cfg.permissions.allow_paths = {"~"};
    McpServerConfig sc;
    sc.name = "srv";
    sc.timeout_s = 120;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--no-sandbox")) sc.sandbox = false;
        else if (!strcmp(argv[i], "--network")) sc.network = true;
        else if (!strcmp(argv[i], "--writable") && i + 1 < argc) sc.writable.push_back(argv[++i]);
    }
    if (argc - i < 3) {
        std::fprintf(stderr, "usage: live_mcp [--no-sandbox] [--network] [--writable P] <tool> <args-json> <command...>\n");
        return 2;
    }
    std::string tool = argv[i++];
    json args = json::parse(argv[i++]);
    for (; i < argc; i++) sc.command.push_back(argv[i]);
    cfg.mcp.servers.push_back(sc);

    ToolRegistry reg;
    mcp::Manager mgr(cfg, reg);
    mgr.start();
    mgr.wait_ready(60000);
    for (auto & s : mgr.status())
        std::printf("server %s: %s protocol=%s name=%s sandbox=%s (%s) tools=%d filtered=%d %s\n", s.name.c_str(), s.state.c_str(),
                    s.protocol.c_str(), s.server_name.c_str(), s.sandboxed ? "yes" : "no", s.sandbox_note.c_str(),
                    s.tools_registered, s.tools_filtered, s.error.c_str());
    for (const Tool * t : reg.list()) std::printf("tool %s: %s\n", t->name.c_str(), t->description.c_str());
    ToolContext ctx;
    ctx.cwd = ".";
    ctx.cfg = &cfg;
    ctx.session = "smoke";
    ToolResult r = reg.call("srv__" + tool, args, ctx);
    std::printf("ok=%d output_length=%zu\n--- head ---\n%s\n", (int) r.ok, r.output.size(), r.output.substr(0, 600).c_str());
    return r.ok ? 0 : 1;
}
