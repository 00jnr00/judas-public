// Artifact tools (lead only): artifact_create, artifact_update, artifact_read, artifact_list, artifact_render.
// The artifacts live in <data_dir>/artifacts (src/artifacts); the Artifacts screens show the same store.
#pragma once

#include "tools/tools.h"

namespace judas {

void register_artifact_tools(ToolRegistry & reg);

}  // namespace judas
