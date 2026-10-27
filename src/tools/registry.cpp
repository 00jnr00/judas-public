// ToolRegistry: storage, lookup and the error-capturing call wrapper.
#include "tools/tools.h"

#include <algorithm>

#include "mcp/schema.h"

namespace judas {

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
