// One JSON type for all of Judas (nlohmann, vendored by llama.cpp). Ordered so tool schemas keep key order.
#pragma once
#include <nlohmann/json.hpp>

namespace judas {
using json = nlohmann::ordered_json;
}
