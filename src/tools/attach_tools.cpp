// view_image and screenshot: put an image in front of the model through an [[image:ID|name]] marker.
// The agent expands markers into ChatMessage::images before each request (vision) or into OCR text (no vision).
#include "attach/attach.h"
#include "tools/tools.h"

namespace judas {
namespace {

ToolResult fail(const std::string & m) { return {false, m, false}; }

ToolResult t_view_image(const json & a, ToolContext & ctx) {
    if (!ctx.cfg) return fail("no configuration available");
    std::string resolved;
    if (std::string d = check_read_path(ctx, a.at("path").get<std::string>(), resolved); !d.empty()) return fail(d);
    if (std::string f = safety_floor_path(ctx, resolved); !f.empty()) return fail(f);
    AttachmentStore store(*ctx.cfg);
    Attachment att;
    try {
        att = store.ingest_file(resolved);
    } catch (const std::exception & e) {
        return fail(std::string("cannot load ") + resolved + ": " + e.what());
    }
    if (att.kind != "image") return fail(resolved + " is not an image (" + att.mime + "); use read_file for text");
    return {true, store.message_block(att) + "\nImage loaded: " + att.name + " (" + std::to_string(att.bytes) + " bytes).", false};
}

ToolResult t_screenshot(const json & a, ToolContext & ctx) {
    if (!ctx.cfg) return fail("no configuration available");
    if (ctx.autonomous) return fail("a background agent may not take screenshots");
    const std::string & mode = ctx.cfg->permissions.mode;
    if (mode != "full-auto") {
        if (!ctx.approve) return fail("not approved: screenshot needs approval (permission mode " + mode + ") and no approver is available");
        if (!ctx.approve("screenshot of the screen")) return fail("not approved: screenshot was declined");
    }
    bool region = a.value("region", false);
    std::string err;
    std::string png = capture_screenshot(region, err);
    if (png.empty()) return fail(err == "no-grim" ? "screenshot unavailable: grim is not installed" : "screenshot failed: " + err);
    AttachmentStore store(*ctx.cfg);
    Attachment att;
    try {
        att = store.ingest(png, "screenshot.png", "image/png");
    } catch (const std::exception & e) {
        return fail(std::string("screenshot failed: ") + e.what());
    }
    return {true, store.message_block(att) + "\nScreenshot taken (" + std::to_string(att.bytes) + " bytes).", false};
}

}  // namespace

void register_attach_tools(ToolRegistry & reg) {
    Tool v;
    v.name = "view_image";
    v.description = "Look at an image file (png, jpg, gif, bmp, webp). The image is shown to you on your next step "
                    "(or its text is read out if you have no vision).";
    v.parameters = {{"type", "object"},
                    {"properties", {{"path", {{"type", "string"}, {"description", "Image path, relative to the working directory or absolute"}}}}},
                    {"required", json::array({"path"})}};
    v.run = t_view_image;
    reg.add(std::move(v));

    Tool s;
    s.name = "screenshot";
    s.description = "Take a screenshot of the user's screen (needs approval unless full-auto). region=true lets the user pick an area.";
    s.parameters = {{"type", "object"},
                    {"properties", {{"region", {{"type", "boolean"}, {"description", "Pick a region instead of the whole screen"}}}}},
                    {"required", json::array()}};
    s.run = t_screenshot;
    reg.add(std::move(s));
}

}  // namespace judas
