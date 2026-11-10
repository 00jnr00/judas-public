// ToolRegistry: storage, lookup and the error-capturing call wrapper.
#include "tools/tools.h"

#include <algorithm>

#include "mcp/schema.h"

namespace judas {

const char * const kHealthTaintMessage =
    "health data is in this chat, so tools that send anything off this machine are off for this session";

std::string location_taint_refusal(const std::string & tool) {
    if (tool != "memory_append" && tool != "skill_manage") return "";
    return "Refused: this chat has the user's location in it, and a location is never saved to memory or skills (" + tool +
           "). Start a new chat to use it.";
}

std::string health_taint_refusal(const std::string & tool, const std::string & source) {
    static const char * const gated[] = {"memory_append", "skill_manage", "web_search", "web_fetch"};
    bool hit = !source.empty();   // MCP servers may be cloud
    for (const char * g : gated) if (tool == g) hit = true;
    if (!hit) return "";
    return std::string("Refused: ") + kHealthTaintMessage + " (" + tool + "). Start a new chat to use it.";
}

void ToolRegistry::add(Tool t) {
    std::lock_guard lk(mu_);
    for (auto & p : tools_) {
        if (p->name == t.name) {
            retired_.push_back(std::move(p));
            p = std::make_unique<Tool>(std::move(t));
            return;
        }
    }
    tools_.push_back(std::make_unique<Tool>(std::move(t)));
}

bool ToolRegistry::remove(const std::string & name) {
    std::lock_guard lk(mu_);
    for (auto it = tools_.begin(); it != tools_.end(); ++it) {
        if ((*it)->name == name) {
            retired_.push_back(std::move(*it));
            tools_.erase(it);
            return true;
        }
    }
    return false;
}

const Tool * ToolRegistry::find(const std::string & name) const {
    std::lock_guard lk(mu_);
    for (auto & p : tools_) {
        if (p->name == name) return p.get();
    }
    return nullptr;
}

std::vector<const Tool *> ToolRegistry::list() const {
    std::lock_guard lk(mu_);
    std::vector<const Tool *> out;
    for (auto & p : tools_) out.push_back(p.get());
    return out;
}

std::vector<const Tool *> ToolRegistry::select(const std::vector<std::string> & names) const {
    if (names.empty()) return list();
    std::vector<const Tool *> out;
    for (auto & n : names) {
        if (const Tool * t = find(n)) out.push_back(t);
    }
    return out;
}

ToolResult ToolRegistry::call(const std::string & name, const json & args, ToolContext & ctx) const {
    const Tool * t = find(name);
    if (!t) {
        std::string names;
        for (const Tool * p : list()) names += (names.empty() ? "" : ", ") + p->name;
        return {false, "unknown tool '" + name + "'. Available tools: " + names, false};
    }
    if (t->grant && !ctx.granted)
        return {false, "Refused: " + name + " runs only when the user explicitly asks for it in their message. Carry on with your own tools.", false};
    if (t->grant && ctx.health_tainted) return {false, std::string("Refused: ") + kHealthTaintMessage + " (" + name + ").", false};
    if (ctx.health_tainted) {
        std::string why = health_taint_refusal(name, t->source);
        if (!why.empty()) return {false, why, false};
    }
    if (ctx.location_used) {
        std::string why = location_taint_refusal(name);
        if (!why.empty()) return {false, why, false};
    }
    if (!args.is_object() && !args.is_null()) return {false, "arguments for " + name + " must be a JSON object", false};
    json a = args.is_null() ? json::object() : args;

    // Required arguments from the schema
    std::vector<std::string> missing;
    if (t->parameters.is_object() && t->parameters.contains("required") && t->parameters["required"].is_array()) {
        for (auto & r : t->parameters["required"]) {
            std::string k = r.get<std::string>();
            if (!a.contains(k) || a[k].is_null()) missing.push_back(k);
        }
    }
    if (!missing.empty()) {
        std::string m;
        for (auto & k : missing) m += (m.empty() ? "" : ", ") + k;
        return {false, "missing required argument(s) for " + name + ": " + m, false};
    }
    // Full schema check (types, enums, bounds). The message goes back as the tool result so the model retries.
    if (std::string bad = mcp::validate_args(t->parameters, a); !bad.empty()) return {false, name + ": " + bad, false};
    try {
        return t->run(a, ctx);
    } catch (const std::exception & e) {
        return {false, name + " failed: " + e.what(), false};
    } catch (...) {
        return {false, name + " failed: unknown error", false};
    }
}

}  // namespace judas
