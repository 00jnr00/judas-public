// project_info and project_list: the lead agent's view of the user's projects (see projects/projects.h).
#pragma once

#include "store/store.h"
#include "tools/tools.h"

namespace judas {

void register_project_tools(ToolRegistry & reg, Store & store);

}  // namespace judas
