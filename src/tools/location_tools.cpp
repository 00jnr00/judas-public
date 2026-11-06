#include "tools/location_tools.h"

#include "location/location.h"
#include "store/store.h"

namespace judas {

void register_location_tools(ToolRegistry & reg) {
    Tool t;
    t.name = "location_get";
    t.description = "Where the user is right now: the latest position his device shared (only when he turned on "
                    "\"Share my location with Judas\" in Settings), how old it is, which device it came from and the "
                    "nearest place name. If nothing is shared it says so; then ask him to turn it on. After this call "
                    "the chat can no longer write memory or skills.";
    t.parameters = {{"type", "object"}, {"properties", json::object()}};
    t.run = [](const json &, ToolContext & ctx) -> ToolResult {
        auto d = location::describe(location::state(), now_ms());
        if (d.found && ctx.note_location) ctx.note_location();
        return {true, d.text, false};
    };
    reg.add(std::move(t));
}

}  // namespace judas
