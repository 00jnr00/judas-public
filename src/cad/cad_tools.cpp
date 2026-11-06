// See cad_tools.h.
#include "cad/cad_tools.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace judas {
namespace {

ToolResult to_tool(const CadResult & r) { return {r.ok, r.text, false}; }
json obj(json props, json required = json::array()) {
    return {{"type", "object"}, {"properties", std::move(props)}, {"required", std::move(required)}};
}
json str(const char * d) { return {{"type", "string"}, {"description", d}}; }
json objt(const char * d) { return {{"type", "object"}, {"description", d}}; }

}  // namespace

bool register_cad_tools(ToolRegistry & reg, std::shared_ptr<CadBackend> be, std::string * why) {
    std::string w;
    if (!be || !be->available(w)) {
        if (why) *why = be ? w : "no backend";
        return false;
    }
    // Sessions get their own live model (cad_backend_for); calls without a session (tests, CLI) use the registered one.
    auto reg_be = be;
    auto add = [&](const char * name, const char * desc, json params, bool writes,
                   std::function<ToolResult(const json &, ToolContext &)> run) {
        Tool t;
        t.name = name;
        t.description = desc;
        t.parameters = std::move(params);
        t.writes = writes;
        t.run = std::move(run);
        reg.add(std::move(t));
    };
    add("cad_state", "Current CAD model: parts, timeline of features, parameters, bodies. Call first.", obj(json::object()), false,
        [reg_be](const json &, ToolContext & ctx) { auto be = ctx.session.empty() ? reg_be : cad_backend_for(ctx.session); std::lock_guard<std::mutex> lk(cad_kernel_mutex()); return to_tool(be->state()); });
    add("cad_new_part", "Start a new empty part. Units are mm and degrees.", obj({{"name", str("part name")}}, json::array({"name"})), false,
        [reg_be](const json & a, ToolContext & ctx) { auto be = ctx.session.empty() ? reg_be : cad_backend_for(ctx.session); std::lock_guard<std::mutex> lk(cad_kernel_mutex()); return to_tool(be->new_part(a.at("name").get<std::string>())); });
    add("cad_sketch", "Create a sketch on a plane and return its closed profiles (ids like s1.p1; a profile with holes is one profile). spec = {plane: \"XY\"|\"YZ\"|\"XZ\"|\"b1.top\" (a planar face: top bottom left right front back or +X..-Z, stable; or a numbered face \"b1.f3\"; origin at the face centre, z = outward normal)|{face, offset}, offset: mm, entities: [...], points: {name: [x,y]}}. Entities: {type:line, from:[x,y], to:[x,y]}, {type:rect, corner|center:[x,y], size:[w,h]}, {type:circle, center, radius|diameter}, {type:arc, center, radius, start_angle, end_angle} (counter-clockwise, degrees), {type:arc3, from, via, to}, {type:polyline, points:[[x,y],...], closed:bool}. Any number can be a parameter expression string such as \"wall*2\". Chains must close within 0.0001 mm. Constraints are not supported.",
        obj({{"spec", objt("plane, points, entities, constraints")}}, json::array({"spec"})), false,
        [reg_be](const json & a, ToolContext & ctx) { auto be = ctx.session.empty() ? reg_be : cad_backend_for(ctx.session); std::lock_guard<std::mutex> lk(cad_kernel_mutex()); return to_tool(be->sketch(a.at("spec"))); });
    add("cad_feature", "Add a feature to the timeline as {type, ...}; numbers may be parameter expressions. extrude {profile:\"s1.p1\"|sketch:\"s1\" (default: last sketch), distance, direction: normal|reverse|symmetric, mode: new|join|cut|intersect (default join, new when no body), body}. revolve {profile|sketch, axis: sketch_x|sketch_y|X|Y|Z|{from:[u,v],to:[u,v]}, angle (default 360), mode}. box {size:[x,y,z], origin|center, mode}. cylinder {radius|diameter, height, origin (base centre), axis, mode}. sphere {radius|diameter, center, mode}. boolean {op: join|cut|intersect, target:\"b1\", tool:\"b2\"}. fillet {edges:[\"b1.e5\", {face:\"b1.f3\"} (all edges of a face), \"all\"], radius}. chamfer {edges, distance}. hole {face:\"b1.top\" (or another direction name; numbered faces shift after each feature), diameter, depth (omit = through), at:[u,v] on the face from its centre or [x,y,z], cbore_diameter, cbore_depth}. shell {remove:[faces], thickness, body}. Name faces by direction (\"b1.top\", \"b1.-X\") whenever you can: numbered face and edge references come from cad_state and shift after every feature and parameter change, so read cad_state again before using one.",
        obj({{"spec", objt("feature fields; type is required")}}, json::array({"spec"})), false,
        [reg_be](const json & a, ToolContext & ctx) { auto be = ctx.session.empty() ? reg_be : cad_backend_for(ctx.session); std::lock_guard<std::mutex> lk(cad_kernel_mutex()); return to_tool(be->feature(a.at("spec"))); });
    add("cad_param", "Create or change a named parameter (mm or an expression) so dimensions change in one place.",
        obj({{"name", str("parameter name")}, {"expr", str("value or expression")}}, json::array({"name", "expr"})), false,
        [reg_be](const json & a, ToolContext & ctx) { auto be = ctx.session.empty() ? reg_be : cad_backend_for(ctx.session); std::lock_guard<std::mutex> lk(cad_kernel_mutex()); return to_tool(be->set_param(a.at("name").get<std::string>(), a.at("expr").get<std::string>())); });
    add("cad_measure", "Measure. refs entries: {} or empty = whole part (volume, area, bbox, centre of mass); {body:\"b1\", density: g/cm3} adds mass in g; {face:\"b1.f3\"}; {edge:\"b1.e5\"}; {between:[a, b]} minimum distance, each a body, face, edge or [x,y,z]; {section:{plane:\"XY\", offset:mm}, body} gives area and perimeter of the cut.",
        obj({{"refs", {{"type", "array"}, {"items", {{"type", "object"}}}, {"description", "entity refs; empty = whole part"}}}}), false,
        [reg_be](const json & a, ToolContext & ctx) { auto be = ctx.session.empty() ? reg_be : cad_backend_for(ctx.session); std::lock_guard<std::mutex> lk(cad_kernel_mutex()); return to_tool(be->measure(a.value("refs", json::array()))); });
    add("cad_export", "Export the part to a new file in the project (stl or step). Refuses to overwrite.",
        obj({{"fmt", str("stl | step")}, {"path", str("output path inside the project; must not exist")},
             {"tolerance", {{"type", "number"}, {"description", "STL linear deflection in mm (default 0.05)"}}}}, json::array({"fmt", "path"})), true,
        [reg_be](const json & a, ToolContext & ctx) -> ToolResult {
            auto be = ctx.session.empty() ? reg_be : cad_backend_for(ctx.session);
            std::string fmt = a.at("fmt").get<std::string>();
            if (fmt != "stl" && fmt != "step") return {false, "fmt must be stl or step", false};
            std::string resolved;
            if (std::string d = check_write_path(ctx, a.at("path").get<std::string>(), resolved); !d.empty()) return {false, "refused: " + d, false};
            std::error_code ec;
            if (fs::exists(resolved, ec)) return {false, resolved + " already exists; pick a new name", false};
            if (ctx.journal) ctx.journal->record(ctx.checkpoint, resolved);
            fs::create_directories(fs::path(resolved).parent_path(), ec);
            json opts = json::object();
            if (a.contains("tolerance") && a["tolerance"].is_number()) opts["tolerance"] = a["tolerance"];
            std::lock_guard<std::mutex> lk(cad_kernel_mutex());
            return to_tool(be->export_opts(fmt, resolved, opts));
        });
    add("cad_undo", "Undo the last CAD change.", obj(json::object()), false,
        [reg_be](const json &, ToolContext & ctx) { auto be = ctx.session.empty() ? reg_be : cad_backend_for(ctx.session); std::lock_guard<std::mutex> lk(cad_kernel_mutex()); return to_tool(be->undo()); });
    return true;
}

}  // namespace judas
