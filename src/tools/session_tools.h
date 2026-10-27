// session_search: keyword search across past conversations through the store's FTS5 index.
#pragma once

#include "store/store.h"
#include "tools/tools.h"

namespace judas {

void register_session_tools(ToolRegistry & reg, Store & store);

}  // namespace judas
