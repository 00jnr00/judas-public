#include "tools/project_tools.h"

#include "projects/projects.h"

namespace judas {

static std::string clip(const std::string & s, size_t n) { return s.size() > n ? s.substr(0, n) + "..." : s; }

void register_project_tools(ToolRegistry & reg, Store & store) {
    {
        Tool t;
        t.name = "project_info";
        t.description = "The project this chat belongs to: its name, folder and instructions. Says so when the chat "
                        "is not in a project (then it is just a working folder).";
        t.parameters = {{"type", "object"}, {"properties", json::object()}};
        t.run = [&store](const json &, ToolContext & ctx) -> ToolResult {
            auto p = projects::of_session(store, ctx.session);
            if (!p) return {true, "This chat is not in a project. Working folder: " + ctx.cwd, false};
            std::string out = "Project: " + p->name + "\nId: " + p->id + "\nFolder: " + p->folder +
                              "\nKind: " + (p->kind == "made" ? "folder made by Judas" : "existing folder") + "\nInstructions: ";
            out += p->instructions.empty() ? "(none)" : "\n" + p->instructions;
            return {true, out, false};
        };
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "project_list";
        t.description = "List the user's projects (name, folder, first line of the instructions), newest first.";
        t.parameters = {{"type", "object"}, {"properties", json::object()}};
        t.run = [&store](const json &, ToolContext &) -> ToolResult {
            auto all = store.list_projects();
            if (all.empty()) return {true, "No projects yet. The user can make one from the project picker.", false};
            std::string out;
            for (auto & p : all) {
                std::string first = p.instructions.substr(0, p.instructions.find('\n'));
                out += "- " + p.name + " (" + p.id + "): " + p.folder + (first.empty() ? "" : "; " + clip(first, 120)) + "\n";
            }
            return {true, out, false};
        };
        reg.add(std::move(t));
    }
}

}  // namespace judas
