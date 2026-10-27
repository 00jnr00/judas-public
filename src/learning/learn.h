// The /learn prompt: one user turn that tells the live agent to gather the named sources and author or extend
// a reusable skill. Shared by the TUI command and POST /api/learn so every surface sends identical text.
#pragma once

#include <string>

namespace judas {

// Build the [/learn] prompt for a request. An empty (or whitespace) request reviews the current conversation.
std::string build_learn_prompt(const std::string & request);

}  // namespace judas
