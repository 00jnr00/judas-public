// Terminal UI entry point. A client of the daemon's HTTP API (docs/API.md): the transcript scrolls in the
// terminal's normal scrollback (no alternate screen, no mouse capture), the live area at the bottom holds
// the running tools, the input box and the status line. See docs/TUI.md.
#pragma once
#include <string>

#include "core/config.h"

namespace judas {

struct TuiOptions {
    std::string session;   // open this session (id or unique prefix)
    bool resume_last = false;  // open the most recent session
    std::string cwd;       // working directory sent with messages (default: current)
};

// Runs the interactive TUI against the daemon at cfg.server.port. Returns a process exit code.
int run_tui(const Config & cfg, const TuiOptions & opts = {});

}  // namespace judas
