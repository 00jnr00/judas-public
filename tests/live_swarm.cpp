// Live check of swarm_map against a running swarm instance: live_swarm <swarm-url> <dir> <glob>
#include <cstdio>

#include "tools/swarm_tools.h"

using namespace judas;

int main(int argc, char ** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: live_swarm URL DIR GLOB\n"); return 2; }
    Config cfg;
    cfg.permissions.allow_paths = {argv[2]};
    cfg.swarm.enabled = true;
    cfg.swarm.url = argv[1];
    cfg.swarm.max_parallel = 8;
    ToolRegistry reg;
    register_swarm_tool(reg);
    ToolContext ctx;
    ctx.cwd = argv[2];
    ctx.cfg = &cfg;
    ctx.progress = [](const json & d) { std::printf("progress %s\n", d.dump().c_str()); };
    ToolResult r = reg.call("swarm_map",
                            {{"instruction", "Give the topic of this document in three words. Reply with the words only."},
                             {"items", json::array({argv[3]})}},
                            ctx);
    std::printf("ok=%d\n%s\n", r.ok, r.output.c_str());
    return r.ok ? 0 : 1;
}
