// Artifacts: versioned storage (never overwrites), path safety, kinds, the in-binary STL/GLB/SVG renderers.
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <set>
#include <string>
#include <thread>

#include "artifacts/artifacts.h"
#include "artifacts/raster.h"
#include "artifacts/render.h"
#include "tools/artifact_tools.h"
#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

static std::string read_file_text(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

// A 2 x 2 x 2 cube centered on the origin, as ASCII STL (12 triangles).
static std::string cube_stl() {
    const float c[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
    const int f[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4}, {2, 3, 7}, {2, 7, 6}, {1, 2, 6}, {1, 6, 5}, {3, 0, 4}, {3, 4, 7}};
    std::string s = "solid cube\n";
    for (auto & t : f) {
        s += "facet normal 0 0 0\nouter loop\n";
        for (int k = 0; k < 3; k++)
            s += "vertex " + std::to_string(c[t[k]][0]) + " " + std::to_string(c[t[k]][1]) + " " + std::to_string(c[t[k]][2]) + "\n";
        s += "endloop\nendfacet\n";
    }
    return s + "endsolid cube\n";
}

static std::string le32(uint32_t v) { return std::string((const char *) &v, 4); }

// a one-triangle GLB (Y-up): positions (0,0,0) (1,0,0) (0,1,0)
static std::string tiny_glb() {
    float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::string bin((const char *) pos, 36);
    std::string js = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                     R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
                     R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
                     R"("bufferViews":[{"buffer":0,"byteLength":36}],"buffers":[{"byteLength":36}]})";
    while (js.size() % 4) js += ' ';
    std::string out = "glTF" + le32(2) + le32((uint32_t) (12 + 8 + js.size() + 8 + bin.size()));
    out += le32((uint32_t) js.size()) + "JSON" + js + le32((uint32_t) bin.size()) + std::string("BIN\0", 4) + bin;
    return out;
}

static void test_paths() {
    CHECK(ArtifactStore::clean_path("index.html") == "index.html");
    CHECK(ArtifactStore::clean_path("css/site.css") == "css/site.css");
    CHECK(ArtifactStore::clean_path("../x").empty());
    CHECK(ArtifactStore::clean_path("a/../../x").empty());
    CHECK(ArtifactStore::clean_path("/etc/passwd").empty());
    CHECK(ArtifactStore::clean_path("a//b").empty());
    CHECK(ArtifactStore::clean_path(".hidden").empty());
    CHECK(ArtifactStore::clean_path("a/.git/x").empty());
    CHECK(ArtifactStore::clean_path("a\\b").empty());
    CHECK(ArtifactStore::clean_path("a b").empty());
    CHECK(ArtifactStore::clean_path("v3/x.js").empty());   // would collide with a version URL
    CHECK(ArtifactStore::clean_path("vendor/x.js") == "vendor/x.js");
    CHECK(ArtifactStore::valid_id("a1b2c3d4e5"));
    CHECK(!ArtifactStore::valid_id("../etc"));
    CHECK(!ArtifactStore::valid_id("A1B2"));
    CHECK(!ArtifactStore::valid_id("ab"));
}

static void test_store() {
    fs::path base = fs::path(JUDAS_TEST_TMP) / ("judas-artifacts-" + std::to_string(getpid()));
    fs::remove_all(base);
    ArtifactStore st((base / "artifacts").string());
    std::string err;

    auto a = st.create("Demo page", "html", {{"index.html", "<h1>one</h1>"}, {"app.js", "console.log(1)"}}, "sess1", "first draft", err);
    REQUIRE(a.has_value());
    CHECK(a->versions.size() == 1 && a->versions[0].n == 1 && a->versions[0].entry == "index.html");
    CHECK(ArtifactStore::valid_id(a->id));

    // update: new version, the old one is untouched, unchanged files carry forward
    auto b = st.update(a->id, {{"index.html", "<h1>two</h1>"}}, {}, "bigger heading", "sess2", err);
    REQUIRE(b.has_value());
    CHECK(b->versions.size() == 2 && b->versions[1].n == 2 && b->versions[1].note == "bigger heading");
    CHECK(b->versions[1].files.size() == 2);
    CHECK(read_file_text(base / "artifacts" / a->id / "v1" / "index.html") == "<h1>one</h1>");
    CHECK(read_file_text(base / "artifacts" / a->id / "v2" / "index.html") == "<h1>two</h1>");
    CHECK(read_file_text(base / "artifacts" / a->id / "v2" / "app.js") == "console.log(1)");

    std::string bytes, used;
    CHECK(st.read_file(a->id, 0, "", bytes, used, err) && bytes == "<h1>two</h1>" && used == "index.html");
    CHECK(st.read_file(a->id, 1, "", bytes, used, err) && bytes == "<h1>one</h1>");
    CHECK(st.read_file(a->id, 2, "app.js", bytes, used, err) && bytes == "console.log(1)");
    CHECK(!st.read_file(a->id, 9, "", bytes, used, err));
    CHECK(!st.read_file(a->id, 1, "../meta.json", bytes, used, err));
    CHECK(!st.read_file(a->id, 1, "../../x", bytes, used, err));
    CHECK(!st.read_file(a->id, 1, "nothere.js", bytes, used, err));
    CHECK(!st.read_file("../" + a->id, 1, "", bytes, used, err));

    // removal makes a version without the file; older versions keep it
    auto c = st.update(a->id, {}, {"app.js"}, "drop js", "", err);
    REQUIRE(c.has_value());
    CHECK(c->versions[2].files.size() == 1);
    CHECK(st.read_file(a->id, 2, "app.js", bytes, used, err));
    CHECK(!st.read_file(a->id, 3, "app.js", bytes, used, err));

    // bad input
    CHECK(!st.create("x", "html", {{"../evil.html", "x"}}, "", "", err).has_value());
    CHECK(!st.create("x", "html", {{"readme.txt", "x"}}, "", "", err).has_value());
    CHECK(!st.create("x", "bogus", {{"index.html", "x"}}, "", "", err).has_value());
    CHECK(!st.create("", "html", {{"index.html", "x"}}, "", "", err).has_value());
    CHECK(!st.update("nonexistent1", {{"index.html", "x"}}, {}, "", "", err).has_value());
    CHECK(!st.update(a->id, {{"/abs.html", "x"}}, {}, "", "", err).has_value());
    CHECK(!st.update(a->id, {}, {}, "", "", err).has_value());   // nothing to change

    // generated viewer pages
    auto md = st.create("Notes", "markdown", {{"document.md", "# Hi\n\nbody"}}, "", "", err);
    REQUIRE(md.has_value());
    CHECK(md->versions[0].entry == "index.html");
    CHECK(st.read_file(md->id, 1, "", bytes, used, err) && bytes.find("mdview.js") != std::string::npos && bytes.find("document.md") != std::string::npos);
    CHECK(!st.update(md->id, {{"index.html", "<script>evil</script>"}}, {}, "", "", err).has_value());   // the viewer page is not the model's to replace
    auto m3 = st.create("Cube", "model3d", {{"model.stl", cube_stl()}}, "", "", err);
    REQUIRE(m3.has_value());
    CHECK(st.read_file(m3->id, 1, "", bytes, used, err) && bytes.find("viewer3d.js") != std::string::npos && bytes.find("data-model=\"model.stl\"") != std::string::npos);
    CHECK(!st.create("No model", "model3d", {{"notes.txt", "x"}}, "", "", err).has_value());
    auto sv = st.create("Logo", "svg", {{"image.svg", "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>"}}, "", "", err);
    CHECK(sv.has_value() && sv->versions[0].entry == "image.svg");

    // list / get / query, newest first
    auto all = st.list();
    CHECK(all.size() == 4);
    CHECK(st.list("cube").size() == 1 && st.list("cube")[0].id == m3->id);
    CHECK(st.list("MODEL3D").size() == 1);
    CHECK(st.list("zzz").empty());
    auto g = st.get(a->id);
    CHECK(g && g->title == "Demo page" && g->versions.size() == 3 && g->session == "sess1");
    CHECK(!st.get("0000000000").has_value());
    json mj = ArtifactStore::meta_json(*g);
    CHECK(mj["latest"] == 3 && mj["versions"].size() == 3);

    // renders are derived data kept beside the versions
    CHECK(st.load_render(a->id, 1).empty());
    CHECK(st.save_render(a->id, 1, "\x89PNG-fake"));
    CHECK(st.load_render(a->id, 1) == "\x89PNG-fake");
    CHECK(!st.save_render(a->id, 7, "x"));

    // delete removes the whole artifact; unknown ids and traversal fail
    CHECK(st.remove(a->id));
    CHECK(!st.get(a->id).has_value());
    CHECK(!fs::exists(base / "artifacts" / a->id));
    CHECK(!st.remove(a->id));
    CHECK(!st.remove("../artifacts"));
    CHECK(fs::exists(base / "artifacts" / m3->id));

    // the in-binary renderer through render_artifact (model3d and svg need no web page)
    RenderResult r = render_artifact(st, m3->id, 0, 320, 240, {"iso"}, 0);
    CHECK(r.ok && r.how == "server" && r.version == 1);
    int w = 0, h = 0;
    CHECK(png_size(r.png, w, h) && w == 320 && h == 240);
    CHECK(!st.load_render(m3->id, 1).empty());
    RenderResult rs = render_artifact(st, sv->id, 0, 0, 0, {}, 0);
    CHECK(rs.ok && png_size(rs.png, w, h));
    // html with no web page connected fails cleanly and quickly
    auto h2 = st.create("Page", "html", {{"index.html", "<p>x</p>"}}, "", "", err);
    RenderResult rh = render_artifact(st, h2->id, 0, 400, 300, {}, 200);
    CHECK(!rh.ok && !rh.error.empty());
    fs::remove_all(base);
}

static const unsigned char * px(const Image & im, int x, int y) { return &im.rgba[((size_t) y * im.w + x) * 4]; }

static bool is_bg(const unsigned char * p) { return p[0] > 215 && p[1] > 215 && p[2] > 215; }

static void test_raster() {
    Mesh cube;
    std::string err;
    REQUIRE(load_stl(cube_stl(), cube, err));
    CHECK(cube.triangles() == 12);
    Image im = render_mesh(cube, 256, 256, {"front"});
    CHECK(im.w == 256 && im.h == 256 && im.rgba.size() == 256u * 256 * 4);
    // front view of a cube: a square in the middle, background in the corners
    CHECK(!is_bg(px(im, 128, 128)));
    CHECK(!is_bg(px(im, 100, 150)));
    CHECK(is_bg(px(im, 3, 3)) && is_bg(px(im, 252, 3)) && is_bg(px(im, 3, 252)) && is_bg(px(im, 252, 252)));
    // the silhouette stays inside the frame margin: the left edge columns are background
    CHECK(is_bg(px(im, 10, 128)) && is_bg(px(im, 245, 128)));
    // foreground area is a sizeable square (the view is framed to the part, so a front-view square fills most of the tile)
    long fg = 0;
    for (int y = 0; y < im.h; y++) for (int x = 0; x < im.w; x++) if (!is_bg(px(im, x, y))) fg++;
    CHECK(fg > 256 * 256 * 0.5 && fg < 256 * 256 * 0.85);

    // iso shows three differently shaded faces
    Image iso = render_mesh(cube, 256, 256, {"iso"});
    CHECK(!is_bg(px(iso, 128, 128)));
    std::set<int> shades;
    for (int y = 0; y < iso.h; y++) for (int x = 0; x < iso.w; x += 3) if (!is_bg(px(iso, x, y))) shades.insert(px(iso, x, y)[0] / 12);
    CHECK(shades.size() >= 3);

    // four views tile into one image of the asked size; each tile has the part in its middle
    Image sheet = render_mesh(cube, 400, 300, default_views());
    CHECK(sheet.w == 400 && sheet.h == 300);
    CHECK(!is_bg(px(sheet, 100, 75)) && !is_bg(px(sheet, 300, 75)) && !is_bg(px(sheet, 100, 225)) && !is_bg(px(sheet, 300, 225)));

    // PNG round trip: a valid header with the right size
    std::string png = encode_png(im);
    int w = 0, h = 0;
    CHECK(png.size() > 100 && png_size(png, w, h) && w == 256 && h == 256);
    CHECK(!png_size("not a png at all, not a png at all", w, h));

    // binary STL with the same cube
    std::string bin(80, ' ');
    bin += le32(12);
    for (size_t t = 0; t < cube.triangles(); t++) {
        float nrm[3] = {0, 0, 0};
        bin.append((const char *) nrm, 12);
        bin.append((const char *) &cube.tri[t * 9], 36);
        bin += std::string(2, '\0');
    }
    Mesh b2;
    CHECK(load_stl(bin, b2, err) && b2.triangles() == 12);
    CHECK(!load_stl("hello", b2, err));
    CHECK(!load_stl("solid x\nendsolid x\n", b2, err));

    // GLB: the triangle loads (Y-up to Z-up) and renders
    Mesh g;
    CHECK(load_glb(tiny_glb(), g, err) && g.triangles() == 1);
    CHECK(!load_glb("glTF-broken", g, err));
    Image gi = render_mesh(g, 128, 128, {"front"});
    bool any = false;
    for (int y = 0; y < gi.h && !any; y++) for (int x = 0; x < gi.w; x++) if (!is_bg(px(gi, x, y))) { any = true; break; }
    CHECK(any);

    // SVG: a red square on white
    Image sv;
    CHECK(render_svg("<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'><rect x='25' y='25' width='50' height='50' fill='#ff0000'/></svg>",
                     200, 200, sv, err));
    CHECK(sv.w == 200 && sv.h == 200);
    CHECK(px(sv, 100, 100)[0] > 240 && px(sv, 100, 100)[1] < 20 && px(sv, 100, 100)[2] < 20);
    CHECK(px(sv, 5, 5)[0] == 255 && px(sv, 5, 5)[1] == 255 && px(sv, 5, 5)[2] == 255);
    CHECK(!render_svg("this is not svg", 100, 100, sv, err));
}

static void test_broker() {
    // a capture delivered while artifact_render waits answers it
    auto & br = RenderBroker::instance();
    br.set_notifier([&](const json & req) {
        std::string id = req["id"].get<std::string>();
        int ver = req["version"].get<int>();
        std::thread([&br, id, ver] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); br.submit(id, ver, "PNGDATA"); }).detach();
    });
    std::string png, err;
    CHECK(br.request("abcd1234ef", 2, 100, 100, 3000, png, err) && png == "PNGDATA");
    br.set_notifier([](const json &) {});
    CHECK(!br.request("abcd1234ef", 2, 100, 100, 100, png, err));
    br.set_notifier(nullptr);
    CHECK(!br.request("abcd1234ef", 2, 100, 100, 100, png, err));
}


static void test_tools() {
    fs::path base = fs::path(JUDAS_TEST_TMP) / ("judas-artifact-tools-" + std::to_string(getpid()));
    fs::remove_all(base);
    Config cfg;
    cfg.data_dir = (base / "data").string();
    ToolRegistry reg;
    register_artifact_tools(reg);
    ToolContext ctx;
    ctx.cfg = &cfg;
    ctx.cwd = base.string();
    ctx.session = "sess-x";
    CHECK(reg.find("artifact_create") && reg.find("artifact_update") && reg.find("artifact_read") && reg.find("artifact_list") && reg.find("artifact_render"));

    auto c = reg.call("artifact_create", {{"title", "Cube"}, {"kind", "model3d"}, {"files", {{"model.stl", cube_stl()}}}, {"note", "unit cube"}}, ctx);
    REQUIRE(c.ok);
    size_t at = c.output.find("[[artifact:");
    REQUIRE(at != std::string::npos);
    std::string id = c.output.substr(at + 11, 10);
    CHECK(ArtifactStore::valid_id(id) && c.output.find("|Cube|model3d|1]]") != std::string::npos);

    auto r = reg.call("artifact_render", {{"id", id}}, ctx);
    CHECK(r.ok && r.output.find("[[image:") == 0 && r.output.find("Rendered inside Judas") != std::string::npos);
    auto r2 = reg.call("artifact_render", {{"id", id}, {"views", {"front"}}, {"width", 300}, {"height", 200}}, ctx);
    CHECK(r2.ok && r2.output.find("300x200") != std::string::npos);
    CHECK(!reg.call("artifact_render", {{"id", "nope000000"}}, ctx).ok);
    CHECK(!reg.call("artifact_render", {{"id", id}, {"version", 5}}, ctx).ok);

    auto u = reg.call("artifact_update", {{"id", id}, {"files", {{"notes.txt", "second"}}}, {"note", "add notes"}}, ctx);
    CHECK(u.ok && u.output.find("version 2") != std::string::npos);
    auto rd = reg.call("artifact_read", {{"id", id}, {"version", 1}}, ctx);
    CHECK(rd.ok && rd.output.find("model.stl") != std::string::npos && rd.output.find("notes.txt") == std::string::npos);
    auto rd2 = reg.call("artifact_read", {{"id", id}, {"path", "notes.txt"}}, ctx);
    CHECK(rd2.ok && rd2.output.find("second") != std::string::npos);
    CHECK(!reg.call("artifact_read", {{"id", id}, {"path", "../meta.json"}}, ctx).ok);
    CHECK(reg.call("artifact_list", {{"query", "cube"}}, ctx).output.find(id) != std::string::npos);
    CHECK(reg.call("artifact_list", {{"query", "zzz"}}, ctx).output.find("no artifacts") == 0);

    // bad requests refuse with a clear message
    CHECK(!reg.call("artifact_create", {{"title", "x"}, {"kind", "html"}, {"files", {{"../up.html", "x"}}}}, ctx).ok);
    CHECK(!reg.call("artifact_create", {{"title", "x"}, {"kind", "html"}}, ctx).ok);
    CHECK(!reg.call("artifact_update", {{"id", id}}, ctx).ok);
    // content: the plain body for markdown/svg/html (the session that lost its report sent exactly this)
    auto md = reg.call("artifact_create", {{"kind", "markdown"}, {"content", "# Insert holes\n\nM6: 8.0 mm [1]\n"}, {"note", "first"}}, ctx);
    REQUIRE(md.ok);
    CHECK(md.output.find("\"Insert holes\"") != std::string::npos);   // title from the first heading
    std::string mid = md.output.substr(md.output.find("[[artifact:") + 11, 10);
    auto mu = reg.call("artifact_update", {{"id", mid}, {"content", "# Insert holes\n\nM6: 8.0 mm [1], M5: 6.4 mm [2]\n"}}, ctx);
    CHECK(mu.ok && mu.output.find("version 2") != std::string::npos);
    auto mr = reg.call("artifact_read", {{"id", mid}, {"path", "report.md"}}, ctx);
    CHECK(mr.ok && mr.output.find("M5: 6.4") != std::string::npos);
    auto sv = reg.call("artifact_create", {{"title", "Sketch"}, {"content", "<svg xmlns='http://www.w3.org/2000/svg'/>"}}, ctx);
    CHECK(sv.ok && sv.output.find("|svg|1]]") != std::string::npos);   // kind inferred
    CHECK(!reg.call("artifact_create", {{"kind", "model3d"}, {"content", "solid x"}}, ctx).ok);
    // source file: copied in from disk through the read-path policy
    fs::create_directories(base);
    std::ofstream(base / "part.stl") << cube_stl();
    auto s = reg.call("artifact_create", {{"title", "From disk"}, {"kind", "model3d"}, {"source", (base / "part.stl").string()}, {"source_name", "model.stl"}}, ctx);
    CHECK(s.ok);
    // html without a web page: a clear error that includes the source
    auto h = reg.call("artifact_create", {{"title", "Page"}, {"kind", "html"}, {"files", {{"index.html", "<p>unique-marker</p>"}}}}, ctx);
    REQUIRE(h.ok);
    std::string hid = h.output.substr(h.output.find("[[artifact:") + 11, 10);
    RenderBroker::instance().set_notifier(nullptr);
    auto hr = reg.call("artifact_render", {{"id", hid}}, ctx);
    CHECK(!hr.ok && hr.output.find("open Judas in a browser tab") != std::string::npos && hr.output.find("unique-marker") != std::string::npos);
    fs::remove_all(base);
}

int main() {
    test_paths();
    test_store();
    test_raster();
    test_broker();
    test_tools();
    TEST_MAIN_END();
}
