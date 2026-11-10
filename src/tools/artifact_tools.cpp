// artifact_create, artifact_update, artifact_read, artifact_list, artifact_render.
#include "tools/artifact_tools.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "artifacts/artifacts.h"
#include "artifacts/raster.h"
#include "artifacts/render.h"
#include "attach/attach.h"

namespace judas {
namespace fs = std::filesystem;
namespace {

ToolResult fail(const std::string & m) { return {false, m, false}; }

std::string clip(const std::string & s, size_t n) { return s.size() > n ? s.substr(0, n) + "\n... (" + std::to_string(s.size() - n) + " more characters)" : s; }

std::string marker(const ArtifactMeta & m, int ver) {
    std::string t = m.title;
    for (auto & c : t) if (c == '|' || c == ']' || c == '[' || c == '\n') c = ' ';
    return "[[artifact:" + m.id + "|" + t + "|" + m.kind + "|" + std::to_string(ver) + "]]";
}

bool looks_text(const std::string & b) {
    size_t n = std::min<size_t>(b.size(), 4096);
    for (size_t i = 0; i < n; i++) if (b[i] == 0) return false;
    return true;
}

// files argument: {"path": "content"}; source: a file on disk copied in under `source_name` (default its own name)
bool collect_files(const json & a, ToolContext & ctx, std::map<std::string, std::string> & files, std::string & err) {
    if (a.contains("files")) {
        if (!a["files"].is_object()) { err = "files must be an object of path: content"; return false; }
        for (auto & [k, v] : a["files"].items()) {
            if (!v.is_string()) { err = "file '" + k + "' must be a string"; return false; }
            std::string s = v.get<std::string>();
            if (s.size() > ArtifactLimits::MAX_TEXT_FILE) { err = "file '" + k + "' is larger than 4 MB; give big binaries through source"; return false; }
            files[k] = std::move(s);
        }
    }
    if (a.contains("source") && a["source"].is_string() && !a["source"].get<std::string>().empty()) {
        std::string resolved;
        if (std::string d = check_allowed_read_path(ctx, a["source"].get<std::string>(), resolved); !d.empty()) { err = d; return false; }
        if (std::string f = safety_floor_path(ctx, resolved); !f.empty()) { err = f; return false; }
        std::error_code ec;
        auto sz = fs::file_size(resolved, ec);
        if (ec) { err = "cannot read " + resolved; return false; }
        if (sz > ArtifactLimits::MAX_BINARY_FILE) { err = "source file is larger than 64 MB"; return false; }
        std::ifstream in(resolved, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string name = a.value("source_name", "");
        if (name.empty()) name = fs::path(resolved).filename().string();
        files[name] = ss.str();
    }
    return true;
}

// The one text file a single-file kind keeps its body in: what a plain `content` argument fills.
std::string default_name(const std::string & kind) {
    if (kind == "markdown") return "report.md";
    if (kind == "svg") return "index.svg";
    if (kind == "html") return "index.html";
    return "";
}
std::string ext_of_kind(const std::string & kind) {
    return kind == "markdown" ? ".md" : kind == "svg" ? ".svg" : kind == "html" ? ".html" : "";
}
// Models send the body as `content` far more often than as files:{name: body}. Accept it for the text kinds:
// on create it goes to the kind's default name, on update it replaces the artifact's main text file.
bool add_content(const json & a, const std::string & kind, const std::string & existing, std::map<std::string, std::string> & files,
                 std::string & err) {
    if (!a.contains("content") || !a["content"].is_string()) return true;
    std::string body = a["content"].get<std::string>();
    if (body.empty()) return true;
    std::string name = !existing.empty() ? existing : default_name(kind);
    if (name.empty()) { err = "content works for markdown, svg and html; give a " + kind + " through source or files"; return false; }
    if (body.size() > ArtifactLimits::MAX_TEXT_FILE) { err = "content is larger than 4 MB"; return false; }
    files[name] = std::move(body);
    return true;
}
// A missing kind is inferred from what was sent; a missing title from the first markdown heading or <title>.
std::string infer_kind(const json & a, const std::map<std::string, std::string> & files) {
    for (auto & [k, v] : files) {
        std::string e = fs::path(k).extension().string();
        if (e == ".md") return "markdown";
        if (e == ".svg") return "svg";
        if (e == ".html") return "html";
        if (e == ".stl" || e == ".glb") return "model3d";
        if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".gif" || e == ".webp") return "image";
    }
    std::string c = a.value("content", "");
    size_t i = c.find_first_not_of(" \n\t");
    if (i != std::string::npos && c.compare(i, 4, "<svg") == 0) return "svg";
    if (i != std::string::npos && c[i] == '<') return "html";
    return c.empty() ? "" : "markdown";
}
std::string infer_title(const std::map<std::string, std::string> & files) {
    for (auto & [k, v] : files) {
        std::istringstream in(v);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("# ", 0) == 0) return line.substr(2, 120);
            if (auto p = line.find("<title>"); p != std::string::npos) {
                auto q = line.find("</title>", p);
                if (q != std::string::npos) return line.substr(p + 7, std::min<size_t>(q - p - 7, 120));
            }
        }
    }
    return "";
}

ToolResult t_create(const json & a, ToolContext & ctx) {
    if (!ctx.cfg) return fail("artifact_create: no configuration available");
    std::map<std::string, std::string> files;
    std::string err;
    if (!collect_files(a, ctx, files, err)) return fail("artifact_create: " + err);
    std::string kind = a.value("kind", "");
    if (kind.empty()) {
        kind = infer_kind(a, files);
    }
    if (kind.empty()) return fail("artifact_create: give kind (html, svg, markdown, model3d, image) and the body as content, files or source");
    if (!add_content(a, kind, "", files, err)) return fail("artifact_create: " + err);
    std::string title = a.value("title", "");
    if (title.empty()) title = infer_title(files);
    if (title.empty()) title = "Untitled " + kind;
    ArtifactStore store(*ctx.cfg);
    auto m = store.create(title, kind, files, ctx.session, a.value("note", ""), err);
    if (!m) return fail("artifact_create: " + err);
    return {true,
            "Created artifact " + m->id + " \"" + m->title + "\" (" + m->kind + "), version 1, " + std::to_string(m->versions[0].files.size()) +
                " file(s). The user opens it in the Artifacts screen or at /a/" + m->id + "/. Look at it yourself with artifact_render.\n" +
                marker(*m, 1),
            false};
}

ToolResult t_update(const json & a, ToolContext & ctx) {
    if (!ctx.cfg) return fail("artifact_update: no configuration available");
    std::map<std::string, std::string> files;
    std::string err;
    if (!collect_files(a, ctx, files, err)) return fail("artifact_update: " + err);
    if (a.contains("content")) {
        ArtifactStore s0(*ctx.cfg);
        auto cur = s0.get(a.value("id", ""));
        if (!cur || !cur->latest()) return fail("artifact_update: no artifact with that id (artifact_list shows them)");
        std::string existing, want = ext_of_kind(cur->kind);
        for (auto & f : cur->latest()->files)
            if (!want.empty() && fs::path(f.path).extension().string() == want) { existing = f.path; break; }
        if (!add_content(a, cur->kind, existing, files, err)) return fail("artifact_update: " + err);
    }
    std::vector<std::string> rm;
    if (a.contains("remove") && a["remove"].is_array())
        for (auto & r : a["remove"]) if (r.is_string()) rm.push_back(r.get<std::string>());
    ArtifactStore store(*ctx.cfg);
    auto m = store.update(a.value("id", ""), files, rm, a.value("note", ""), ctx.session, err, a.value("title", ""));
    if (!m) return fail("artifact_update: " + err);
    int n = m->versions.back().n;
    return {true,
            "Updated artifact " + m->id + " \"" + m->title + "\" to version " + std::to_string(n) + " (version " + std::to_string(n - 1) +
                " is kept; send only the changed files, the rest carry forward).\n" + marker(*m, n),
            false};
}

ToolResult t_read(const json & a, ToolContext & ctx) {
    if (!ctx.cfg) return fail("artifact_read: no configuration available");
    ArtifactStore store(*ctx.cfg);
    auto m = store.get(a.value("id", ""));
    if (!m) return fail("artifact_read: no artifact with that id (artifact_list shows them)");
    int ver = a.contains("version") && a["version"].is_number() ? a["version"].get<int>() : 0;
    const ArtifactVersion * v = m->version(ver);
    if (!v) return fail("artifact_read: no such version");
    std::string out = "[" + m->id + "] \"" + m->title + "\" (" + m->kind + "), version " + std::to_string(v->n) + " of " + std::to_string(m->latest()->n);
    if (!v->note.empty()) out += " (" + v->note + ")";
    out += "\nfiles:";
    for (auto & f : v->files) out += "\n  " + f.path + " (" + std::to_string(f.bytes) + " bytes)";
    std::string path = a.value("path", ""), bytes, used, err;
    // the generated viewer page is not interesting; show the document or the entry the author wrote
    if (path.empty()) {
        for (auto & f : v->files)
            if (f.path != "index.html" || m->kind == "html") { if (path.empty() || f.path == "index.html" || f.path == "document.md" || f.path == "image.svg") path = f.path; }
    }
    if (!store.read_file(m->id, v->n, path, bytes, used, err)) return fail("artifact_read: " + err);
    if (!looks_text(bytes)) return {true, out + "\n\n" + used + " is binary; not shown", false};
    return {true, out + "\n\n--- " + used + " ---\n" + clip(bytes, 60000), false};
}

ToolResult t_list(const json & a, ToolContext & ctx) {
    if (!ctx.cfg) return fail("artifact_list: no configuration available");
    ArtifactStore store(*ctx.cfg);
    auto ms = store.list(a.value("query", ""));
    if (ms.empty()) return {true, "no artifacts" + std::string(a.value("query", "").empty() ? "" : " match that query"), false};
    std::string out;
    for (auto & m : ms) {
        if (!out.empty()) out += '\n';
        out += "[" + m.id + "] " + m.title + " | " + m.kind + " | " + std::to_string(m.versions.size()) + " version(s)";
    }
    return {true, clip(out, 20000), false};
}

ToolResult t_render(const json & a, ToolContext & ctx) {
    if (!ctx.cfg) return fail("artifact_render: no configuration available");
    ArtifactStore store(*ctx.cfg);
    std::string id = a.value("id", "");
    int ver = a.contains("version") && a["version"].is_number() ? a["version"].get<int>() : 0;
    int w = a.contains("width") && a["width"].is_number() ? a["width"].get<int>() : 1024;
    int h = a.contains("height") && a["height"].is_number() ? a["height"].get<int>() : 768;
    std::vector<std::string> views;
    if (a.contains("views") && a["views"].is_array())
        for (auto & v : a["views"]) if (v.is_string()) views.push_back(v.get<std::string>());
    RenderResult r = render_artifact(store, id, ver, w, h, views);
    if (!r.ok) {
        std::string msg = "artifact_render: " + r.error;
        auto m = store.get(id);
        if (m && (m->kind == "html" || m->kind == "markdown")) {
            msg += ". html and markdown are captured by a Judas web page: open Judas in a browser tab (desktop or phone) and try again.";
            std::string bytes, used, e;
            if (store.read_file(id, r.version, "", bytes, used, e)) msg += "\nSource of " + used + " meanwhile:\n" + clip(bytes, 12000);
        }
        return fail(msg);
    }
    AttachmentStore att_store(*ctx.cfg);
    Attachment att;
    try {
        att = att_store.ingest(r.png, "artifact-" + id + "-v" + std::to_string(r.version) + ".png", "image/png");
    } catch (const std::exception & e) {
        return fail(std::string("artifact_render: cannot store the image: ") + e.what());
    }
    int pw = 0, ph = 0;
    png_size(r.png, pw, ph);
    std::string note = r.how == "server" ? "Rendered inside Judas" : "Captured by an open Judas web page";
    auto m = store.get(id);
    if (m && m->kind == "model3d") note += "; views tiled: " + std::string(views.empty() ? "iso (top left), front (top right), right (bottom left), top (bottom right)" : "as requested") + "; axes X red, Y green, Z blue, Z up";
    return {true, att_store.message_block(att) + "\n" + note + ". Version " + std::to_string(r.version) + ", " + std::to_string(pw) + "x" + std::to_string(ph) + ".", false};
}

}  // namespace

void register_artifact_tools(ToolRegistry & reg) {
    const json files_schema = {{"type", "object"}, {"additionalProperties", {{"type", "string"}}},
                               {"description", "Map of relative file path to full text content, e.g. {\"index.html\": \"...\"}"}};
    {
        Tool t;
        t.name = "artifact_create";
        t.description =
            "Create a persistent artifact the user can open later in the Artifacts screen (desktop or phone): a web page, document, diagram "
            "or 3D view. kind: html (needs index.html; may add css/js files; runs sandboxed with no network), svg (one .svg), markdown (one .md), "
            "model3d (an .stl or .glb, given as files text for ASCII STL or as source path of a file on disk; shown with an orbit viewer), "
            "image (png/jpg/gif/webp via source). Every change later is a new version via artifact_update; nothing is overwritten. "
            "For markdown, svg and html the simplest call is title + kind + content (the text). Then call artifact_render to look at your own work and fix it.";
        t.parameters = {{"type", "object"},
                        {"properties",
                         {{"title", {{"type", "string"}}},
                          {"kind", {{"type", "string"}, {"enum", {"html", "svg", "markdown", "model3d", "image"}}}},
                          {"content", {{"type", "string"}, {"description", "The body of a markdown, svg or html artifact (the simplest way to give it)"}}},
                          {"files", files_schema},
                          {"source", {{"type", "string"}, {"description", "Path of a file on disk to copy in (STL, GLB, image)"}}},
                          {"source_name", {{"type", "string"}, {"description", "Name to store the source file under, e.g. model.stl"}}},
                          {"note", {{"type", "string"}, {"description", "One line: what this version is"}}}}},
                        {"required", json::array()}};
        t.run = t_create;
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "artifact_update";
        t.description =
            "Save a new version of an artifact. Send only the files that changed (they replace the same path); the other files carry forward. "
            "remove lists paths to drop. note is a one-line change note. The earlier versions stay and the user can open any of them.";
        t.parameters = {{"type", "object"},
                        {"properties",
                         {{"id", {{"type", "string"}}},
                          {"content", {{"type", "string"}, {"description", "New full body of a markdown, svg or html artifact"}}},
                          {"files", files_schema},
                          {"source", {{"type", "string"}}},
                          {"source_name", {{"type", "string"}}},
                          {"remove", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                          {"title", {{"type", "string"}, {"description", "Optional new title"}}},
                          {"note", {{"type", "string"}}}}},
                        {"required", {"id"}}};
        t.run = t_update;
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "artifact_read";
        t.description = "Read an artifact: its versions and files, and the text of one file (default the main one). version 0 or omitted = latest.";
        t.parameters = {{"type", "object"},
                        {"properties", {{"id", {{"type", "string"}}}, {"version", {{"type", "integer"}}}, {"path", {{"type", "string"}}}}},
                        {"required", {"id"}}};
        t.run = t_read;
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "artifact_list";
        t.description = "List artifacts (id, title, kind, version count), newest first. query filters by title or kind.";
        t.parameters = {{"type", "object"}, {"properties", {{"query", {{"type", "string"}}}}}};
        t.run = t_list;
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "artifact_render";
        t.description =
            "Render a picture of an artifact so you can look at it with vision and iterate. model3d (STL/GLB), svg and png images render inside Judas "
            "at once; model3d shows views iso, front, right, top tiled (views may list up to 4 of iso, front, back, left, right, top, bottom). "
            "html and markdown are captured by an open Judas web page, so a browser tab with Judas must be open; if none answers you get the source instead.";
        t.parameters = {{"type", "object"},
                        {"properties",
                         {{"id", {{"type", "string"}}},
                          {"version", {{"type", "integer"}, {"description", "Omit for the latest"}}},
                          {"width", {{"type", "integer"}, {"description", "Default 1024"}}},
                          {"height", {{"type", "integer"}, {"description", "Default 768"}}},
                          {"views", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                        {"required", {"id"}}};
        t.run = t_render;
        reg.add(std::move(t));
    }
}

}  // namespace judas
