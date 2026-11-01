// Strata backend (built with -DJUDAS_STRATA=ON). See docs/STRATA-BACKEND.md.
#pragma once

#include <memory>

#include "core/config.h"
#include "engine/engine.h"

namespace judas {

// Starts the `strata --serve` engine for cfg.active_model() (backend = "strata") and returns once it is READY.
// Throws std::runtime_error when the tokenizer, the chat template or the engine cannot be started.
std::unique_ptr<Engine> make_strata_engine(const Config & cfg);

}  // namespace judas
