// See filter.h.
#include "mcp/filter.h"

#include "mcp/launch.h"

namespace judas::mcp {

namespace {

bool entry_matches(const std::string & e, const Tool & t) {
    if (e == t.name) return true;
    if (!t.source.empty() && t.source.rfind("mcp:", 0) == 0) {
        if (e == "mcp" || e == "mcp:*") return true;
        if (e.rfind("mcp:", 0) == 0 && e == t.source) return true;
    }
    if (e.find_first_of("*?") != std::string::npos) return glob_match(e, t.name);
    return false;
}

}  // namespace

bool role_allows(const std::vector<std::string> & role_tools, const Tool & t) {
    bool any_positive = false;
    bool included = false;
    for (const auto & raw : role_tools) {
        if (!raw.empty() && raw[0] == '!') {
            if (entry_matches(raw.substr(1), t)) return false;
        } else {
            any_positive = true;
            if (entry_matches(raw, t)) included = true;
        }
    }
    return !any_positive || included;
}

}  // namespace judas::mcp
