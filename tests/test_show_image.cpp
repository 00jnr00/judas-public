// show_image (URL fetch against a local HTTP server, file paths inside/outside allow_paths, non-image
// rejection, size cap), web_search category mapping, and the rich-output prompt examples.
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "agent/agent.h"
#include "attach/attach.h"
#include "cpp-httplib/httplib.h"
#include "test.h"
#include "tools/tools.h"

namespace fs = std::filesystem;
using namespace judas;

static const char * PNG_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

static std::string slurp(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
static void spit(const fs::path & p, const std::string & s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static bool has(const std::string & h, const std::string & n) { return h.find(n) != std::string::npos; }

int main() {
    fs::path root = fs::path(JUDAS_TEST_TMP) / ("show-image-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    std::string png = base64_decode(PNG_B64);
    REQUIRE(png.size() > 20);

    // ---- a local web server: images, redirects, junk, and a fake SearXNG ----
    std::mutex mu;
    std::string last_categories, last_query;
    httplib::Server web;
    web.Get("/ok.png", [&](const httplib::Request &, httplib::Response & r) { r.set_content(png, "image/png"); });
    web.Get("/redir", [&](const httplib::Request &, httplib::Response & r) { r.status = 302; r.set_header("Location", "/ok.png"); });
    web.Get("/redir-rel", [&](const httplib::Request &, httplib::Response & r) { r.status = 302; r.set_header("Location", "ok.png"); });
    web.Get("/redir-ftp", [&](const httplib::Request &, httplib::Response & r) { r.status = 302; r.set_header("Location", "ftp://example.com/a.png"); });
    web.Get("/loop", [&](const httplib::Request &, httplib::Response & r) { r.status = 302; r.set_header("Location", "/loop"); });
    web.Get("/page.html", [&](const httplib::Request &, httplib::Response & r) { r.set_content("<html>hi</html>", "text/html"); });
    web.Get("/lie.png", [&](const httplib::Request &, httplib::Response & r) { r.set_content("this is not a png at all", "image/png"); });
    web.Get("/pic.svg", [&](const httplib::Request &, httplib::Response & r) { r.set_content("<svg xmlns='http://www.w3.org/2000/svg'/>", "image/svg+xml"); });
    web.Get("/missing.png", [&](const httplib::Request &, httplib::Response & r) { r.status = 404; });
    web.Get("/big.png", [&](const httplib::Request &, httplib::Response & r) { r.set_content(png + std::string(2 << 20, 'x'), "image/png"); });
    web.Get("/search", [&](const httplib::Request & req, httplib::Response & r) {
        {
            std::lock_guard<std::mutex> g(mu);
            last_categories = req.get_param_value("categories");
            last_query = req.get_param_value("q");
        }
        json res = json::array();
        if (last_categories == "images") {
            res.push_back({{"title", "Red fox"}, {"url", "https://example.com/fox"}, {"img_src", "http://127.0.0.1/fox.jpg"}, {"thumbnail_src", "http://127.0.0.1/t.jpg"}});
            res.push_back({{"title", "No image here"}, {"url", "https://example.com/none"}});
        } else {
            res.push_back({{"title", "Fox facts"}, {"url", "https://example.com/facts"}, {"content", "Foxes are canids."}});
        }
        r.set_content(json({{"results", res}}).dump(), "application/json");
    });
    int port = web.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { web.listen_after_bind(); });
    while (!web.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    // ---- config: allowed dir, a secret dir, a 1 MB attachment cap ----
    fs::path allowed = root / "allowed", outside = root / "outside", secret = allowed / "secret", work = root / "work";
    fs::create_directories(work);
    spit(allowed / "pic.png", png);
    spit(allowed / "notes.txt", "just text");
    spit(allowed / "fake.png", "text pretending to be a png");
    spit(allowed / "huge.png", png + std::string(2 << 20, 'x'));
    spit(secret / "key.png", png);
    spit(outside / "pic.png", png);
    Config cfg;
    cfg.data_dir = (root / "data").string();
    cfg.permissions.allow_paths = {allowed.string()};
    cfg.permissions.secret_paths = {secret.string()};
    cfg.attachments.max_bytes = 1 << 20;
    cfg.search.searxng = base;
    ToolRegistry reg;
    register_builtin_tools(reg);
    register_attach_tools(reg);
    ToolContext ctx;
    ctx.cwd = work.string();
    ctx.cfg = &cfg;
    AttachmentStore store(cfg);

    auto md_id = [&](const ToolResult & r, const std::string & cap) {
        std::string pre = "![" + cap + "](/api/attachments/";
        size_t a = r.output.find(pre);
        if (a == std::string::npos) return std::string();
        size_t b = r.output.find(')', a);
        return r.output.substr(a + pre.size(), b - a - pre.size());
    };

    // ---- URL fetch ----
    {
        auto r = reg.call("show_image", {{"source", base + "/ok.png"}, {"caption", "A pixel"}}, ctx);
        CHECK(r.ok);
        std::string id = md_id(r, "A pixel");
        CHECK(id.size() == 16 && valid_attachment_id(id));
        auto att = store.get(id);
        REQUIRE(att.has_value());
        CHECK(att->kind == "image");
        CHECK(store.read_bytes(id) == png);
        CHECK(has(r.output, "attachment id: " + id));
        // a caption cannot break out of the markdown
        auto c = reg.call("show_image", {{"source", base + "/ok.png"}, {"caption", "a [b]\nc"}}, ctx);
        CHECK(c.ok && has(c.output, "![a  b  c](/api/attachments/"));
        // no caption: falls back to the file name
        auto n = reg.call("show_image", {{"source", base + "/ok.png"}}, ctx);
        CHECK(n.ok && has(n.output, "![ok.png](/api/attachments/"));
    }
    {   // redirects to http(s) are followed; to anything else, refused; loops end
        auto r = reg.call("show_image", {{"source", base + "/redir"}}, ctx);
        CHECK(r.ok && has(r.output, "/api/attachments/"));
        auto rl = reg.call("show_image", {{"source", base + "/redir-rel?x=1"}}, ctx);   // relative Location resolves against the current URL
        CHECK(rl.ok && has(rl.output, "/api/attachments/"));
        auto f = reg.call("show_image", {{"source", base + "/redir-ftp"}}, ctx);
        CHECK(!f.ok && has(f.output, "non-http"));
        auto l = reg.call("show_image", {{"source", base + "/loop"}}, ctx);
        CHECK(!l.ok && has(l.output, "redirect"));
    }
    {   // not an image
        auto h = reg.call("show_image", {{"source", base + "/page.html"}}, ctx);
        CHECK(!h.ok && has(h.output, "not an image"));
        auto lie = reg.call("show_image", {{"source", base + "/lie.png"}}, ctx);
        CHECK(!lie.ok && has(lie.output, "not a png"));
        auto sv = reg.call("show_image", {{"source", base + "/pic.svg"}}, ctx);
        CHECK(!sv.ok && has(sv.output, "not an image"));
        auto nf = reg.call("show_image", {{"source", base + "/missing.png"}}, ctx);
        CHECK(!nf.ok && has(nf.output, "404"));
        auto ft = reg.call("show_image", {{"source", "ftp://example.com/a.png"}}, ctx);
        CHECK(!ft.ok && has(ft.output, "only http"));
        auto fu = reg.call("show_image", {{"source", "file:///etc/passwd"}}, ctx);
        CHECK(!fu.ok);
    }
    {   // size cap
        auto big = reg.call("show_image", {{"source", base + "/big.png"}}, ctx);
        CHECK(!big.ok && has(big.output, "larger than"));
    }

    // ---- file paths ----
    {
        auto in = reg.call("show_image", {{"source", (allowed / "pic.png").string()}, {"caption", "local"}}, ctx);
        CHECK(in.ok && !md_id(in, "local").empty());
        auto rel = reg.call("show_image", {{"source", "../allowed/pic.png"}, {"caption", "rel"}}, ctx);
        CHECK(rel.ok);   // resolves against the working directory, lands inside allow_paths
        auto out = reg.call("show_image", {{"source", (outside / "pic.png").string()}}, ctx);
        CHECK(!out.ok && has(out.output, "outside the allowed paths"));
        auto sec = reg.call("show_image", {{"source", (secret / "key.png").string()}}, ctx);
        CHECK(!sec.ok && has(sec.output, "secret"));
        auto sh = reg.call("show_image", {{"source", (root / "allowed" / ".." / "outside" / "pic.png").string()}}, ctx);
        CHECK(!sh.ok);
        auto txt = reg.call("show_image", {{"source", (allowed / "notes.txt").string()}}, ctx);
        CHECK(!txt.ok && has(txt.output, "not a png"));
        auto fk = reg.call("show_image", {{"source", (allowed / "fake.png").string()}}, ctx);
        CHECK(!fk.ok);
        auto hg = reg.call("show_image", {{"source", (allowed / "huge.png").string()}}, ctx);
        CHECK(!hg.ok && has(hg.output, "larger than"));
        auto ms = reg.call("show_image", {{"source", (allowed / "none.png").string()}}, ctx);
        CHECK(!ms.ok);
        // a symlink inside the allowed dir that points outside is refused
        std::error_code ec;
        fs::create_symlink(outside / "pic.png", allowed / "link.png", ec);
        if (!ec) {
            auto ln = reg.call("show_image", {{"source", (allowed / "link.png").string()}}, ctx);
            CHECK(!ln.ok);
        }
    }

    // ---- web_search category mapping ----
    {
        auto g = reg.call("web_search", {{"query", "fox"}}, ctx);
        CHECK(g.ok && has(g.output, "Fox facts"));
        { std::lock_guard<std::mutex> l(mu); CHECK(last_categories == "general"); CHECK(last_query == "fox"); }
        auto im = reg.call("web_search", {{"query", "red fox"}, {"category", "images"}}, ctx);
        CHECK(im.ok);
        { std::lock_guard<std::mutex> l(mu); CHECK(last_categories == "images"); CHECK(last_query == "red fox"); }
        CHECK(has(im.output, "Red fox"));
        CHECK(has(im.output, "page: https://example.com/fox"));
        CHECK(has(im.output, "img_src: http://127.0.0.1/fox.jpg"));
        CHECK(!has(im.output, "No image here"));   // image results without an image URL are dropped
        CHECK(has(im.output, "show_image"));
        auto nw = reg.call("web_search", {{"query", "fox"}, {"category", "news"}}, ctx);
        CHECK(nw.ok);
        { std::lock_guard<std::mutex> l(mu); CHECK(last_categories == "news"); }
        auto bad = reg.call("web_search", {{"query", "fox"}, {"category", "videos"}}, ctx);
        CHECK(!bad.ok && has(bad.output, "general, images or news"));
        auto dflt = reg.call("web_search", {{"query", "fox"}, {"category", "general"}}, ctx);
        CHECK(dflt.ok);
        { std::lock_guard<std::mutex> l(mu); CHECK(last_categories == "general"); }
        const Tool * ws = reg.find("web_search");
        REQUIRE(ws != nullptr);
        CHECK(ws->parameters["properties"].contains("category"));
        CHECK(has(ws->description, "show_image"));
    }

    web.stop();
    th.join();

    // ---- the prompt carries the worked examples ----
    {
        std::string p = rich_output_prompt();
        CHECK(has(p, "```swatch"));
        CHECK(has(p, "hex: #"));
        CHECK(has(p, "```cards"));
        CHECK(has(p, "image: /api/attachments/"));
        CHECK(has(p, "show_image"));
        CHECK(has(p, "category \"images\""));
        CHECK(has(p, "![caption](/api/attachments/ID)"));
        CHECK(has(p, "- [ ] item"));
        CHECK(has(p, "home directory"));
        CHECK(!has(p, "\xE2\x80\x94"));   // no em dash
        CHECK(p.size() < 2200);           // token budget
    }
    // ---- the renderer takes a palette of up to four colors ----
    {
        std::string js = slurp(fs::path(JUDAS_TESTS_DIR) / ".." / "web" / "rich.js");
        CHECK(has(js, "rc-palette"));
        CHECK(has(js, "slice(0, 4)"));
    }
    TEST_MAIN_END();
}
