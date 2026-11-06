// view_image and screenshot: put an image in front of the model through an [[image:ID|name]] marker.
// The agent expands markers into ChatMessage::images before each request (vision) or into OCR text (no vision).
#include <chrono>
#include <cstring>
#include <fstream>
#include <regex>

#include "attach/attach.h"
#include "cpp-httplib/httplib.h"
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

// ---------- show_image ----------

constexpr long long SHOW_IMAGE_MAX_BYTES = 15LL << 20;   // upper bound; attachments.max_bytes can only lower it
constexpr int SHOW_IMAGE_MAX_REDIRECTS = 4;
constexpr int SHOW_IMAGE_DEADLINE_S = 30;

// png / jpg / gif / webp by magic bytes ("" for anything else, svg included: it can carry script).
std::string image_magic(const std::string & b) {
    auto starts = [&](const char * p, size_t n) { return b.size() >= n && std::memcmp(b.data(), p, n) == 0; };
    if (starts("\x89PNG\r\n\x1a\n", 8)) return "png";
    if (starts("\xff\xd8\xff", 3)) return "jpg";
    if (starts("GIF87a", 6) || starts("GIF89a", 6)) return "gif";
    if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WEBP", 4) == 0) return "webp";
    return "";
}

std::string clean_caption(std::string c) {
    for (auto & ch : c) if (ch == '[' || ch == ']' || ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    size_t a = c.find_first_not_of(' '), b = c.find_last_not_of(' ');
    c = a == std::string::npos ? "" : c.substr(a, b - a + 1);
    if (c.size() > 120) c.resize(120);
    return c;
}

// Name for the stored copy: the last URL path segment, with an extension that matches the real type.
std::string image_name(const std::string & hint, const std::string & magic) {
    std::string n = hint;
    size_t cut = n.find_first_of("?#");
    if (cut != std::string::npos) n.resize(cut);
    size_t sl = n.find_last_of('/');
    if (sl != std::string::npos) n = n.substr(sl + 1);
    size_t dot = n.rfind('.');
    std::string stem = dot == std::string::npos ? n : n.substr(0, dot);
    std::string ext = dot == std::string::npos ? "" : n.substr(dot + 1);
    for (auto & c : ext) c = (char) std::tolower((unsigned char) c);
    bool ext_ok = ext == magic || (ext == "jpeg" && magic == "jpg");
    if (stem.empty() || stem.size() > 60) stem = "image";
    return stem + "." + (ext_ok ? ext : magic);
}

ToolResult image_markdown(AttachmentStore & store, const std::string & bytes, const std::string & name_hint,
                          const std::string & caption_in) {
    std::string magic = image_magic(bytes);
    if (magic.empty()) return fail("show_image: not a png, jpeg, gif or webp image");
    Attachment att;
    try {
        att = store.ingest(bytes, image_name(name_hint, magic), "image/" + std::string(magic == "jpg" ? "jpeg" : magic));
    } catch (const std::exception & e) {
        return fail(std::string("show_image: cannot store the image: ") + e.what());
    }
    if (att.kind != "image") return fail("show_image: not an image");
    std::string cap = clean_caption(caption_in);
    if (cap.empty()) cap = clean_caption(att.name);
    return {true, "Stored. Paste this line into your reply to show the image to the user:\n![" + cap + "](/api/attachments/" + att.id +
                      ")\nattachment id: " + att.id + " (also usable as `image: /api/attachments/" + att.id + "` in a cards block)",
            false};
}

struct Url { std::string scheme, origin, path; };

bool parse_http_url(const std::string & u, Url & out) {
    static const std::regex re(R"(^(https?)://([^/?#@\s]+)([^\s]*)$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(u, m, re)) return false;
    out.scheme = m[1];
    for (auto & c : out.scheme) c = (char) std::tolower((unsigned char) c);
    out.origin = out.scheme + "://" + m[2].str();
    out.path = m[3].str();
    if (out.path.empty() || out.path[0] != '/') out.path = "/" + out.path;
    return true;
}

ToolResult fetch_image(AttachmentStore & store, const std::string & url0, const std::string & caption) {
    long long cap_bytes = std::min<long long>(SHOW_IMAGE_MAX_BYTES, store.max_bytes());
    auto t0 = std::chrono::steady_clock::now();
    std::string url = url0;
    for (int hop = 0; hop <= SHOW_IMAGE_MAX_REDIRECTS; hop++) {
        Url u;
        if (!parse_http_url(url, u)) return fail("show_image: only http:// and https:// URLs are fetched (got '" + url + "')");
        httplib::Client cli(u.origin);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(15);
        cli.set_follow_location(false);   // redirects are followed by hand so each hop is checked
        std::string body, err;
        bool too_big = false, bad_type = false;
        int status = 0;
        std::string location;
        httplib::Headers hdrs = {{"Accept", "image/*"}, {"User-Agent", "Mozilla/5.0 (compatible; Judas)"}};
        auto res = cli.Get(
            u.path, hdrs,
            [&](const httplib::Response & r) {
                status = r.status;
                location = r.get_header_value("Location");
                if (status != 200) return true;   // body ignored below
                std::string ct = r.get_header_value("Content-Type");
                for (auto & c : ct) c = (char) std::tolower((unsigned char) c);
                if (ct.rfind("image/", 0) != 0 || ct.rfind("image/svg", 0) == 0) { bad_type = true; err = ct.empty() ? "no content type" : ct; return false; }
                std::string cl = r.get_header_value("Content-Length");
                if (!cl.empty() && std::atoll(cl.c_str()) > cap_bytes) { too_big = true; return false; }
                return true;
            },
            [&](const char * data, size_t n) {
                if (status != 200) return true;
                if ((long long) (body.size() + n) > cap_bytes) { too_big = true; return false; }
                if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(SHOW_IMAGE_DEADLINE_S)) return false;
                body.append(data, n);
                return true;
            });
        if (bad_type) return fail("show_image: " + url + " is not an image (content type " + err + ")");
        if (too_big) return fail("show_image: image is larger than the " + std::to_string(cap_bytes >> 20) + " MB limit");
        if (!res) return fail("show_image: could not fetch " + url + " (unreachable, TLS error or timed out)");
        if (status >= 300 && status < 400 && !location.empty()) {
            static const std::regex scheme_re(R"(^[A-Za-z][A-Za-z0-9+.\-]*:)");
            if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) url = location;
            else if (std::regex_search(location, scheme_re)) return fail("show_image: redirect to a non-http location refused");
            else if (location.rfind("//", 0) == 0) url = u.scheme + ":" + location;
            else if (location[0] == '/') url = u.origin + location;
            else {   // relative to the current path (query and fragment dropped)
                std::string dir = u.path.substr(0, u.path.find_first_of("?#"));
                dir.resize(dir.find_last_of('/') + 1);
                url = u.origin + dir + location;
            }
            continue;
        }
        if (status != 200) return fail("show_image: " + url + " returned HTTP " + std::to_string(status));
        if (body.empty()) return fail("show_image: empty response");
        return image_markdown(store, body, u.path, caption);
    }
    return fail("show_image: too many redirects");
}

ToolResult t_show_image(const json & a, ToolContext & ctx) {
    if (!ctx.cfg) return fail("no configuration available");
    std::string src = a.at("source").get<std::string>();
    std::string caption = a.contains("caption") && a["caption"].is_string() ? a["caption"].get<std::string>() : std::string();
    AttachmentStore store(*ctx.cfg);
    size_t b = src.find_first_not_of(" \t\r\n"), e = src.find_last_not_of(" \t\r\n");
    src = b == std::string::npos ? "" : src.substr(b, e - b + 1);
    if (src.empty()) return fail("show_image: source is empty");
    std::string low = src.substr(0, 8);
    for (auto & c : low) c = (char) std::tolower((unsigned char) c);
    if (low.rfind("http://", 0) == 0 || low.rfind("https://", 0) == 0) {
        if (ctx.health_tainted)   // a URL is a request that leaves the machine
            return fail(std::string("show_image: ") + kHealthTaintMessage + " (remote images are not fetched)");
        return fetch_image(store, src, caption);
    }
    if (src.find("://") != std::string::npos) return fail("show_image: only http(s) URLs and file paths are accepted");
    std::string resolved;
    if (std::string d = check_allowed_read_path(ctx, src, resolved); !d.empty()) return fail("show_image: " + d);
    if (std::string f = safety_floor_path(ctx, resolved); !f.empty()) return fail("show_image: " + f);
    std::error_code ec;
    auto sz = std::filesystem::file_size(resolved, ec);
    if (ec) return fail("show_image: cannot read " + resolved);
    long long cap_bytes = std::min<long long>(SHOW_IMAGE_MAX_BYTES, store.max_bytes());
    if ((long long) sz > cap_bytes) return fail("show_image: image is larger than the " + std::to_string(cap_bytes >> 20) + " MB limit");
    std::string bytes;
    {
        std::ifstream in(resolved, std::ios::binary);
        if (!in) return fail("show_image: cannot read " + resolved);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return image_markdown(store, bytes, resolved, caption);
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

    Tool si;
    si.name = "show_image";
    si.description = "Show a real image to the USER inside your reply. source is an http(s) image URL (for example an img_src from "
                     "web_search category=images) or a file path; the server downloads and stores it and returns a markdown line "
                     "![caption](/api/attachments/ID) that you paste into your reply. view_image is only for you; use this for the user.";
    si.parameters = {{"type", "object"},
                     {"properties", {{"source", {{"type", "string"}, {"description", "http(s) image URL or file path"}}},
                                     {"caption", {{"type", "string"}, {"description", "Short caption / alt text"}}}}},
                     {"required", json::array({"source"})}};
    si.run = t_show_image;
    reg.add(std::move(si));

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
