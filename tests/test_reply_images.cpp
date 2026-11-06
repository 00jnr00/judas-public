// fix_reply_images: remote markdown images become attachments via show_image, failed fetches stay as written,
// invented /api/attachments ids are dropped.
#include <unistd.h>

#include <filesystem>
#include <thread>

#include "attach/attach.h"
#include "cpp-httplib/httplib.h"
#include "test.h"
#include "tools/tools.h"

namespace fs = std::filesystem;
using namespace judas;

static const char * PNG_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

int main() {
    fs::path root = fs::path(JUDAS_TEST_TMP) / ("reply-images-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "work");
    std::string png = base64_decode(PNG_B64);

    httplib::Server web;
    web.Get("/ok.png", [&](const httplib::Request &, httplib::Response & r) { r.set_content(png, "image/png"); });
    web.Get("/gone.png", [&](const httplib::Request &, httplib::Response & r) { r.status = 404; });
    int port = web.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { web.listen_after_bind(); });
    while (!web.is_running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::string base = "http://127.0.0.1:" + std::to_string(port);

    Config cfg;
    cfg.data_dir = (root / "data").string();
    cfg.attachments.max_bytes = 1 << 20;
    ToolRegistry reg;
    register_builtin_tools(reg);
    register_attach_tools(reg);
    ToolContext ctx;
    ctx.cwd = (root / "work").string();
    ctx.cfg = &cfg;
    AttachmentStore store(cfg);

    auto fetch = [&](const std::string & url, const std::string & cap) -> std::string {
        auto r = reg.call("show_image", {{"source", url}, {"caption", cap}}, ctx);
        if (!r.ok) return "";
        size_t p = r.output.find("attachment id: ");
        return p == std::string::npos ? "" : "/api/attachments/" + r.output.substr(p + 15, 16);
    };
    auto exists = [&](const std::string & id) { return valid_attachment_id(id) && store.get(id).has_value(); };

    {   // replace
        std::string in = "Here:\n![A pixel](" + base + "/ok.png)\nDone.";
        std::string out = fix_reply_images(in, fetch, exists);
        CHECK(out.find(base) == std::string::npos);
        size_t p = out.find("![A pixel](/api/attachments/");
        REQUIRE(p != std::string::npos);
        std::string id = out.substr(p + 28, 16);
        CHECK(store.get(id).has_value());
        CHECK(out.rfind("Here:\n", 0) == 0 && out.find("\nDone.") != std::string::npos);
        // the stored id now exists, so a second pass leaves the result alone
        CHECK(fix_reply_images(out, fetch, exists) == out);
    }
    {   // fetch failure turns the image into a plain link
        std::string in = "![gone](" + base + "/gone.png) and ![x](https://127.0.0.1:1/none.png)";
        CHECK(fix_reply_images(in, fetch, exists) == "[gone](" + base + "/gone.png) and [x](https://127.0.0.1:1/none.png)");
    }
    {   // invented id: image removed, caption kept
        std::string in = "Look: ![Cyan palette](/api/attachments/0123456789abcdef) ok";
        CHECK(fix_reply_images(in, fetch, exists) == "Look: Cyan palette ok");
        std::string bad = "![a](/api/attachments/nope)";
        CHECK(fix_reply_images(bad, fetch, exists) == "a");
    }
    {   // code fences are not touched; at most 4 fetches per reply
        std::string in = "```\n![c](" + base + "/ok.png)\n```\n";
        CHECK(fix_reply_images(in, fetch, exists) == in);
        std::string many;
        for (int i = 0; i < 6; i++) many += "![i" + std::to_string(i) + "](" + base + "/ok.png?n=" + std::to_string(i) + ")\n";
        std::string out = fix_reply_images(many, fetch, exists);
        size_t left = 0, pos = 0;
        while ((pos = out.find(base, pos)) != std::string::npos) { left++; pos++; }
        CHECK(left == 2);   // the two past the cap stay as they were
        // the same URL twice is fetched once and both copies point at the stored image
        std::string twice = "![a](" + base + "/ok.png?t=1) ![b](" + base + "/ok.png?t=1)";
        std::string o2 = fix_reply_images(twice, fetch, exists);
        CHECK(o2.find(base) == std::string::npos && o2.find("/api/attachments/") != std::string::npos);
    }

    web.stop();
    th.join();
    fs::remove_all(root);
    TEST_MAIN_END();
}
