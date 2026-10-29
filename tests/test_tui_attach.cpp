// TUI attachments and companions: path detection from pasted text, marker collapsing, chips, half-block
// rendering of a tiny sprite, and the companion state machine.
#include <filesystem>
#include <fstream>

#include "core/json.h"
#include "test.h"
#include "tui/attach.h"
#include "tui/sprites.h"
#include "tui/view.h"

using namespace judas::tui;
using judas::json;
namespace fs = std::filesystem;

static void touch(const fs::path & p, const std::string & body = "x") {
    std::ofstream(p, std::ios::binary) << body;
}

int main() {
    fs::path d = fs::path(JUDAS_TEST_TMP) / "tui_attach";
    fs::remove_all(d);
    fs::create_directories(d / "sub dir");
    touch(d / "a.png");
    touch(d / "report.pdf");
    touch(d / "sub dir" / "my photo.jpg");
    touch(d / "it's.txt");
    std::string cwd = d.string();

    {   // shell splitting
        auto t = shell_split("one 'two words' \"three \\\"q\\\"\" four\\ five");
        REQUIRE(t.size() == 4);
        CHECK(t[0] == "one" && t[1] == "two words" && t[2] == "three \"q\"" && t[3] == "four five");
        CHECK(shell_split("  \n ").empty());
        CHECK(percent_decode("a%20b%2Fc") == "a b/c");
        CHECK(url_encode("my file.png") == "my%20file.png");
    }
    {   // dropped files: plain, quoted, backslash-escaped, file:// URI, several at once
        auto p = detect_paths(cwd + "/a.png", cwd);
        REQUIRE(p.size() == 1);
        CHECK(p[0] == cwd + "/a.png");
        p = detect_paths("'" + cwd + "/sub dir/my photo.jpg'", cwd);
        REQUIRE(p.size() == 1);
        CHECK(p[0] == cwd + "/sub dir/my photo.jpg");
        p = detect_paths(cwd + "/sub\\ dir/my\\ photo.jpg\n", cwd);
        CHECK(p.size() == 1);
        p = detect_paths("file://" + cwd + "/sub%20dir/my%20photo.jpg", cwd);
        CHECK(p.size() == 1);
        p = detect_paths(cwd + "/a.png " + cwd + "/report.pdf", cwd);
        CHECK(p.size() == 2);
        p = detect_paths("file://" + cwd + "/a.png\nfile://" + cwd + "/report.pdf\n", cwd);
        CHECK(p.size() == 2);
        p = detect_paths("\"" + cwd + "/it's.txt\"", cwd);
        CHECK(p.size() == 1);
        p = detect_paths("./a.png", cwd);
        CHECK(p.size() == 1);  // ./ is resolved against the TUI cwd
    }
    {   // things that are not (only) paths stay text
        CHECK(detect_paths("hello world", cwd).empty());
        CHECK(detect_paths("a.png", cwd).empty());                      // bare names are text unless /attach
        CHECK(detect_paths(cwd + "/a.png and more", cwd).empty());
        CHECK(detect_paths(cwd + "/missing.png", cwd).empty());
        CHECK(detect_paths(cwd, cwd).empty());                           // a directory
        CHECK(detect_paths("", cwd).empty());
        CHECK(detect_paths("/usr/bin/nonexistent-zzz /etc/hostname", cwd).empty());  // one bad path spoils it
    }
    {   // /attach arguments are lenient: relative names work, bad ones are reported
        std::vector<std::string> miss;
        auto p = parse_attach_args("a.png \"sub dir/my photo.jpg\" nope.txt", cwd, &miss);
        CHECK(p.size() == 2);
        REQUIRE(miss.size() == 1);
        CHECK(miss[0] == "nope.txt");
    }
    {   // collapsing markers
        auto l = collapse_attachments("look at this\n[[image:abc123|photo.png]]\nand\n"
                                      "<attachment name=\"report.pdf\" id=\"i1\" kind=\"pdf\">\nHello\nworld!\n</attachment>\nbye");
        REQUIRE(l.size() == 5);
        CHECK(l[0].text == "look at this" && !l[0].chip);
        CHECK(l[1].text == "\xF0\x9F\x96\xBC photo.png" && l[1].chip);
        CHECK(l[2].text == "and");
        CHECK(l[3].text == "\xF0\x9F\x93\x8E report.pdf (pdf, 12 chars)" && l[3].chip);
        CHECK(l[4].text == "bye");
        // the attachment text never appears
        for (auto & x : l) CHECK(x.text.find("Hello") == std::string::npos);
        // scanned pdf: block with empty text then page images
        l = collapse_attachments("<attachment name=\"scan.pdf\" id=\"s\" kind=\"pdf\">\n\n</attachment>\n[[image:p1|scan.pdf p.1]]\n[[image:p2|scan.pdf p.2]]");
        REQUIRE(l.size() == 3);
        CHECK(l[0].text.find("scan.pdf (pdf, 0 chars)") != std::string::npos);
        CHECK(l[2].text == "\xF0\x9F\x96\xBC scan.pdf p.2");
        // unclosed block collapses to the end; plain text is untouched
        l = collapse_attachments("<attachment name=\"x.txt\" id=\"x\" kind=\"text\">\nabc");
        CHECK(l.size() == 1);
        l = collapse_attachments("a\n\nb\n");
        CHECK(l.size() == 4 && l[3].text.empty());
        // multibyte text counts characters
        l = collapse_attachments("<attachment name=\"u.txt\" id=\"u\" kind=\"text\">\n\xC3\xA9\xC3\xA9\n</attachment>");
        CHECK(l.size() == 1 && l[0].text.find("(text, 2 chars)") != std::string::npos);
    }
    {   // the transcript shows chips, not text
        Stream s(Palette::plain(), 60);
        s.user_message("see\n[[image:z|pic.png]]\n<attachment name=\"n.md\" id=\"n\" kind=\"text\">\nSECRET BODY\n</attachment>");
        std::string all;
        for (auto & r : s.take_committed()) all += strip_ansi(r) + "\n";
        CHECK(all.find("SECRET BODY") == std::string::npos);
        CHECK(all.find("> see") != std::string::npos);
        CHECK(all.find("\xF0\x9F\x96\xBC pic.png") != std::string::npos);
        CHECK(all.find("n.md (text, 11 chars)") != std::string::npos);
        s.user_message("", {"\xF0\x9F\x93\x8E a.pdf (pdf, 5 chars)"});
        all.clear();
        for (auto & r : s.take_committed()) all += strip_ansi(r) + "\n";
        CHECK(all.find("> \xF0\x9F\x93\x8E a.pdf") != std::string::npos);
    }
    {   // chips
        CHECK(human_bytes(512) == "512 B");
        CHECK(human_bytes(2200000) == "2.1 MB");
        PendingAttachment a{"1", "report.pdf", "pdf", 2200000, 10}, b{"2", "photo.png", "image", 5, 0};
        CHECK(chip_text(a, 1) == "[1] \xF0\x9F\x93\x8E report.pdf 2.1 MB");
        CHECK(chip_text(b, 2) == "[2] \xF0\x9F\x96\xBC photo.png");
        auto rows = render_chip_rows({a, b}, 60, Palette::plain());
        REQUIRE(rows.size() == 1);
        CHECK(display_width(rows[0]) <= 60);
        rows = render_chip_rows({a, b}, 30, Palette::plain());
        CHECK(rows.size() == 2);
        CHECK(guess_mime("x.PNG") == "image/png" && guess_mime("noext") == "application/octet-stream");
    }
    {   // half blocks: 2x2 sprite, red over blue on the left, transparent over green on the right
        json j = {{"palette", {{"r", "#ff0000"}, {"b", "#0000ff"}, {"g", "#00ff00"}, {".", "transparent"}}},
                  {"characters", {{"tiny", {{"w", 2}, {"h", 2},
                                            {"frames", {{"idle", json::array({json::array({"rb", ".g"}), json::array({"rr", ".."})})},
                                                        {"happy", json::array({json::array({"gg", "gg"})})}}},
                                            {"fps", {{"idle", 2}}}}}}}};
        Companions c;
        REQUIRE(c.load(j));
        CHECK(c.characters().size() == 1);
        CHECK(c.panel_width() == 2 && c.sprite_rows() == 1 && c.panel_rows() == 2);
        auto rows = render_halfblock(c.characters()[0].frames.at("idle")[0], 2);
        REQUIRE(rows.size() == 1);
        const std::string H = "\xE2\x96\x80", L = "\xE2\x96\x84";
        // left: top red, bottom transparent => fg red upper half; right: top blue, bottom green => fg blue bg green
        CHECK(rows[0] == "\033[0m\033[38;2;255;0;0m" + H + "\033[38;2;0;0;255m\033[48;2;0;255;0m" + H + "\033[0m");
        // transparent top, opaque bottom => lower half block; fully transparent => default-colored space
        SpriteFrame f2 = c.characters()[0].frames.at("idle")[1];
        auto r2 = render_halfblock(f2, 2);
        CHECK(r2[0] == "\033[0m\033[38;2;255;0;0m" + H + "\033[0m\033[38;2;255;0;0m" + H + "\033[0m");
        SpriteFrame f3(2, std::vector<Pixel>(2));
        f3[1][0] = Pixel{true, 1, 2, 3};
        CHECK(render_halfblock(f3, 2)[0] == "\033[0m\033[38;2;1;2;3m" + L + "\033[0m \033[0m");
        // bad input
        Companions bad;
        CHECK(!bad.load(json::object()));
        CHECK(!bad.load(json{{"characters", json::object()}}));
    }
    {   // companion state machine
        json j = {{"palette", {{"r", "#ff0000"}}},
                  {"characters", {{"tiny", {{"w", 1}, {"h", 2}, {"frames", {{"idle", json::array({json::array({"r", "r"})})}}}}}}}};
        Companions c;
        REQUIRE(c.load(j));
        auto ev = [](const char * type, json data, const char * parent = "") {
            return json{{"type", type}, {"agent", "L1"}, {"parent", parent}, {"role", "lead"}, {"data", data}};
        };
        CHECK(c.state(0, false) == "idle");
        c.on_event(ev("reasoning", {{"text", "hm"}}), 10);
        CHECK(c.state(10.5, true) == "think");
        CHECK(c.state(11, false) == "idle");  // no turn running: back to idle
        c.on_event(ev("tool_call", {{"name", "read_file"}}), 12);
        CHECK(c.state(13, true) == "work");
        c.on_event(ev("done", {{"status", "done"}}), 20);
        CHECK(c.state(21, false) == "happy");
        CHECK(c.state(23.5, false) == "idle");
        c.on_event(ev("status", {{"error", "boom"}}), 30);
        CHECK(c.state(31, true) == "sad");
        CHECK(c.state(35, true) == "idle" || c.state(35, true) == "work" || c.state(35, true) == "think");
        c.on_event(ev("done", {{"status", "done"}}, "L1"), 40);  // a worker finishing is not a happy turn
        CHECK(c.state(41, false) == "idle");
        // missing state frames fall back to idle; render has panel_rows rows of equal width
        auto rows = c.render(1.0, false, "", "");
        CHECK((int) rows.size() == c.panel_rows());
        CHECK(display_width(rows[0]) == c.panel_width() || c.panel_width() == 1);
    }
    fs::remove_all(d);
    TEST_MAIN_END();
}
