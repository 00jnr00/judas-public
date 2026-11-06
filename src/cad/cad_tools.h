// cad_* tools: thin wrappers over a CadBackend, for Engineering-mode sessions only.
#pragma once

#include <memory>

#include "cad/cad_backend.h"
#include "tools/tools.h"

namespace judas {

// Registers cad_state, cad_new_part, cad_sketch, cad_feature, cad_param, cad_measure, cad_export, cad_undo when the
// backend is available; registers nothing otherwise (the model then never sees tools that cannot work).
// Returns true when the tools were registered; `why` gets the reason when not.
bool register_cad_tools(ToolRegistry & reg, std::shared_ptr<CadBackend> backend, std::string * why = nullptr);

}  // namespace judas
