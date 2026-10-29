#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "attach/attach.h"
#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

static const char * PNG_B64 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==";

static std::string png_variant(int n) {   // distinct valid-looking PNG bytes: same header, different tail
    std::string p = base64_decode(PNG_B64);
    return p + std::string((size_t) n, 'x');
}

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-attach-test-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root);
    Config cfg;
    cfg.data_dir = (root / "data").string();
    AttachmentStore store(cfg);

    // base64
    for (std::string s : {std::string(""), std::string("a"), std::string("ab"), std::string("abc"), std::string("hello\0world", 11)})
        CHECK(base64_decode(base64_encode(s)) == s);
    CHECK(base64_encode("Man") == "TWFu");
    CHECK(base64_encode("Ma") == "TWE=");
    CHECK(base64_decode("TWFu\nTWE=") == "ManMa");
    CHECK(base64_decode("!!!").empty());
    std::string png = base64_decode(PNG_B64);
    REQUIRE(png.size() > 20);
    CHECK(png.substr(1, 3) == "PNG");

    // markers
    {
        std::string t = "see [[image:0123456789abcdef|shot one.png]] and\n[[image:fedcba9876543210|b]] x [[image:zz|bad]] [[image:0123456789abcdef|no end";
        auto r = find_image_markers(t);
        REQUIRE(r.size() == 2);
        CHECK(r[0].id == "0123456789abcdef" && r[0].name == "shot one.png");
        CHECK(t.substr(r[0].pos, r[0].len) == "[[image:0123456789abcdef|shot one.png]]");
        CHECK(r[1].name == "b");
    }

    // text file
    Attachment t = store.ingest("hello world\nline two\n", "notes.txt");
    CHECK(t.kind == "text");
    CHECK(t.id.size() == 16 && valid_attachment_id(t.id));
    CHECK(t.text == "hello world\nline two\n");
    CHECK(store.ingest("hello world\nline two\n", "other.md").id == t.id);   // same bytes, same id
    CHECK(store.read_bytes(t.id) == "hello world\nline two\n");
    auto g = store.get(t.id);
    REQUIRE(g.has_value());
    CHECK(g->name == "notes.txt" && g->text == t.text && g->bytes == 21);
    CHECK(store.message_block(t) ==
          "<attachment name=\"notes.txt\" id=\"" + t.id + "\" kind=\"text\">\nhello world\nline two\n\n</attachment>");
    json tj = attachment_json(t);
    CHECK(tj["url"] == "/api/attachments/" + t.id && tj["kind"] == "text" && tj["text_chars"] == 21 && !tj["truncated"].get<bool>());
    CHECK(!store.get("../../etc/passwd").has_value());
    CHECK(!store.get("XYZ").has_value());
    CHECK(store.read_bytes("nothex").empty());

    // names are sanitized
    Attachment sn = store.ingest("x = 1\n", "../../evil|name[1].py");
    CHECK(sn.name.find('/') == std::string::npos && sn.name.find('|') == std::string::npos && sn.name.find('[') == std::string::npos);
    CHECK(fs::path(sn.path).parent_path() == fs::path(store.dir()));

    // truncation
    {
        Config c2 = cfg;
        c2.attachments.max_text_chars = 10;
        AttachmentStore s2(c2);
        Attachment big = s2.ingest("0123456789abcdefghij", "big.txt");
        CHECK(big.truncated && big.text == "0123456789");
        CHECK(attachment_json(big)["truncated"].get<bool>());
        bool threw = false;
        c2.attachments.max_bytes = 4;
        AttachmentStore s3(c2);
        try { s3.ingest("toolong", "x.txt"); } catch (const std::exception &) { threw = true; }
        CHECK(threw);
        threw = false;
        try { store.ingest("", "x.txt"); } catch (const std::exception &) { threw = true; }
        CHECK(threw);
    }

    // image
    Attachment im = store.ingest(png, "dot.png");
    CHECK(im.kind == "image" && im.mime == "image/png");
    CHECK(store.ingest(png, "again.png").id == im.id);
    CHECK(store.message_block(im) == "[[image:" + im.id + "|dot.png]]");
    CHECK(store.read_bytes(im.id) == png);
    CHECK(store.ingest(png, "noext").kind == "image");   // by mime-less magic bytes: image, same id

    // doc without helper tools: a docx made by zip is skipped when zip is absent; html is handled in-process
    {
        Attachment h = store.ingest("<html><body><h1>Title</h1><p>Para &amp; more</p><script>x()</script></body></html>", "page.html");
        CHECK(h.kind == "doc");
        CHECK(h.text.find("Title") != std::string::npos && h.text.find("Para & more") != std::string::npos);
        CHECK(h.text.find("x()") == std::string::npos);
    }

    // PDF with a text layer (hand written; pdftotext rebuilds the xref)
    if (tool_available("pdftotext")) {
        std::string pdf =
            "%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
            "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 300 100]/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>endobj\n"
            "4 0 obj<</Length 85>>stream\nBT /F1 12 Tf 10 50 Td (Hello attachments this page has plenty of readable text) Tj ET\nendstream endobj\n"
            "5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\ntrailer<</Root 1 0 R/Size 6>>\n%%EOF\n";
        Attachment p = store.ingest(pdf, "doc.pdf");
        CHECK(p.kind == "pdf");
        if (p.text.empty()) std::fprintf(stderr, "note: hand-written PDF not parsed by pdftotext; text check skipped\n");
        else {
            CHECK(p.text.find("Hello attachments") != std::string::npos);
            CHECK(p.pages.empty());
            CHECK(store.message_block(p).rfind("<attachment name=\"doc.pdf\"", 0) == 0);
        }
    }

    // scanned pdf block format (synthetic): block then one marker line per page
    {
        Attachment a = store.ingest("%PDF-fake", "scan.pdf");
        Attachment pg1 = store.ingest(png_variant(1), "scan.pdf p.1");
        Attachment pg2 = store.ingest(png_variant(2), "scan.pdf p.2");
        a.pages = {pg1.id, pg2.id};
        a.text = "";
        CHECK(store.message_block(a) == "<attachment name=\"scan.pdf\" id=\"" + a.id + "\" kind=\"pdf\">\n\n</attachment>\n[[image:" +
                                            pg1.id + "|scan.pdf p.1]]\n[[image:" + pg2.id + "|scan.pdf p.2]]");
    }

    // expansion without vision
    {
        ChatMessage m{"user", "look: [[image:" + im.id + "|dot.png]] ok"};
        expand_images(store, m, false);
        CHECK(m.images.empty());
        CHECK(m.content.find("[image: dot.png, not shown: the loaded model has no vision]") != std::string::npos);
        CHECK(m.content.find("[[image:") == std::string::npos);
        std::vector<ChatMessage> v = {{"user", "[[image:" + im.id + "|dot.png]]"}};
        expand_request_images(store, v, false);
        CHECK(v[0].images.empty() && v[0].content.rfind("[image: dot.png, not shown", 0) == 0);
    }

    // expansion with vision: newest 8 load, older markers say "shown earlier"
    {
        std::vector<Attachment> imgs;
        for (int i = 0; i < 10; i++) imgs.push_back(store.ingest(png_variant(100 + i), "img" + std::to_string(i) + ".png"));
        std::vector<ChatMessage> msgs;
        for (int i = 0; i < 10; i++) {
            ChatMessage m;
            m.role = i % 3 == 0 ? "tool" : "user";
            m.content = "msg " + std::to_string(i) + " [[image:" + imgs[i].id + "|img" + std::to_string(i) + ".png]]";
            msgs.push_back(m);
        }
        // one message with two markers, the older of the two is the one cut at the boundary
        msgs.push_back({"user", "[[image:" + imgs[0].id + "|a]] and [[image:" + imgs[1].id + "|b]]"});
        auto before = msgs;
        expand_request_images(store, msgs, true);
        int loaded = 0;
        for (auto & m : msgs) loaded += (int) m.images.size();
        CHECK(loaded == 8);
        CHECK(msgs.back().images.size() == 2 && msgs.back().images[0] == store.read_bytes(imgs[0].id));
        CHECK(msgs.back().content == before.back().content);   // markers stay when loaded
        CHECK(msgs[0].images.empty());
        CHECK(msgs[0].content.find("[image: img0.png, shown earlier]") != std::string::npos);
        CHECK(msgs[1].images.empty() && msgs[1].content.find("shown earlier") != std::string::npos);
        CHECK(msgs[4].images.size() == 1 && msgs[3].images.empty());   // 12 markers, newest 8 load: last (2) + msgs 9..4
        CHECK(msgs[9].images.size() == 1 && msgs[9].images[0] == store.read_bytes(imgs[9].id));
        // an unknown id does not touch the filesystem and does not count
        std::vector<ChatMessage> bad = {{"user", "[[image:../../../../etc/pw|x]] [[image:0000000000000000|y]]"}};
        expand_request_images(store, bad, true);
        CHECK(bad[0].images.empty());
        CHECK(bad[0].content.find("not available") != std::string::npos);
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
