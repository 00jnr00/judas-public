// Hooks: user commands run at agent lifecycle events (pre-tool, post-tool, pre-edit, post-edit, turn-end,
// task-done). Each hook gets a JSON object about the event on stdin. A blocking pre-* hook that exits non-zero
// cancels the action. Hooks let the user add lint, test, notify or audit gates without touching core.
#pragma once

#include <string>

#include "core/config.h"
#include "core/json.h"

namespace judas {

struct HookOutcome {
    bool allow = true;        // a blocking pre-* hook exited non-zero -> false (refuse the action)
    std::string reason;       // why it was refused (hook stderr/stdout, trimmed)
};

class Hooks {
public:
    explicit Hooks(const HooksConfig & cfg) : cfg_(cfg) {}
    bool enabled() const { return cfg_.enabled && !cfg_.hooks.empty(); }
    // Run every hook registered for `event` whose `match` (if any) is a substring of `selector` (tool name or
    // path). `payload` is sent as JSON on the hook's stdin. Non-blocking hooks never affect the outcome.
    HookOutcome run(const std::string & event, const std::string & selector, const json & payload) const;

private:
    HooksConfig cfg_;
};

}  // namespace judas
