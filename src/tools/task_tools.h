// Scheduled-task tools: the model can list, create, pause/resume and delete the same tasks as the Tasks
// screen (store tasks table; the server's scheduler runs them). Every change asks the user (approve_strict).
#pragma once

#include "store/store.h"
#include "tools/tools.h"

namespace judas {

void register_task_tools(ToolRegistry & reg, Store & store);

// "YYYY-MM-DD HH:MM", "YYYY-MM-DDTHH:MM" or "HH:MM" (the next time that clock time comes) in local time.
// Returns epoch ms, or 0 when the text does not parse. Exposed for tests.
long long parse_local_time(const std::string & s, long long now_ms);

}  // namespace judas
