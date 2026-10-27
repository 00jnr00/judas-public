// A small JSON Schema checker for tool-call arguments. It covers what tool schemas actually use (type, enum,
// const, required, properties, additionalProperties, items, min/max, length, anyOf/oneOf/allOf) and ignores
// keywords it does not know ($ref, pattern, format, ...), so it can only reject clear mistakes.
#pragma once

#include <string>

#include "core/json.h"

namespace judas::mcp {

// Returns "" when `args` satisfies `schema`, else a short message the model can act on (up to 5 problems,
// each naming the argument path, e.g. "pages[1]: expected integer, got string").
std::string validate_args(const json & schema, const json & args);

}  // namespace judas::mcp
