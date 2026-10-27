// TUI commands: parsing, "/" filtering, argument menus, "@" path completion.
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "test.h"
#include "tui/commands.h"

namespace fs = std::filesystem;
using namespace judas::tui;

int main() {
    CHECK(parse_input("").kind == Parsed::Empty);
    CHECK(parse_input("  \n").kind == Parsed::Empty);
    auto p = parse_input("/Mode  code ");
    CHECK(p.kind == Parsed::Slash && p.name == "mode" && p.arg == "code");
    CHECK(parse_input("/help").name == "help");
    CHECK(parse_input("/usr/bin/ls is here").kind == Parsed::Message);   // a path, not a command
    auto sh = parse_input("!ls -la");
    CHECK(sh.kind == Parsed::Shell && sh.arg == "ls -la");
    CHECK(parse_input("!").kind == Parsed::Empty);
    CHECK(parse_input("hello\nworld").kind == Parsed::Message);

    CHECK(find_command("copy") != nullptr);
    CHECK(find_command("compact") == nullptr);                          // the API has no compact
    auto f = filter_commands("re");
    REQUIRE(!f.empty());
    CHECK(f[0]->name == "resume");                                      // prefix match first
    bool has_sub = false;
    for (auto * c : filter_commands("ume")) has_sub = has_sub || c->name == "resume";
    CHECK(has_sub);                                                   // substring match
    CHECK(filter_commands("").size() == commands().size());
    CHECK(filter_commands("zzz").empty());
    auto fz = filter_commands("atrm");                                   // fuzzy: auto-resume
    REQUIRE(!fz.empty());
    CHECK(fz[0]->name == "auto-resume");

    MenuContext ctx;
    ctx.cwd = "/tmp";
    ctx.models = {"big", "small"};
    auto m = compute_menu("/", 1, ctx);
    CHECK(m.kind == Menu::Slash && m.items.size() == commands().size());
    m = compute_menu("/mo", 3, ctx);
    REQUIRE(m.kind == Menu::Slash);
    CHECK(m.items[0].value == "/mode " && m.items[1].value == "/model");
    m = compute_menu("/mode ", 6, ctx);
    CHECK(m.kind == Menu::Arg && m.items.size() == 3 && m.items[0].value == "/mode chat");
    m = compute_menu("/mode cod", 9, ctx);
    CHECK(m.kind == Menu::Arg && m.items.size() == 1 && m.items[0].value == "/mode code" + std::string());
    CHECK(compute_menu("/mode code", 10, ctx).items.empty() || compute_menu("/mode code", 10, ctx).kind != Menu::Slash);
    m = compute_menu("/model sma", 10, ctx);
    CHECK(m.kind == Menu::Arg && m.items[0].label == "small");
    CHECK(!compute_menu("hello", 5, ctx).active());
    CHECK(!compute_menu("/zzz", 4, ctx).active());

    fs::path root = JUDAS_TEST_TMP "/judas-tui-cmd-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root / "src" / "sub");
    std::ofstream(root / "src" / "main.cpp") << "x";
    std::ofstream(root / "src" / "mod.h") << "x";
    std::ofstream(root / "README.md") << "x";
    std::ofstream(root / ".hidden") << "x";
    auto c1 = complete_path(root.string(), "");
    CHECK(c1.size() == 2 && c1[0] == "src/" && c1[1] == "README.md");   // dirs first, dotfiles hidden
    auto c2 = complete_path(root.string(), "src/m");
    CHECK(c2.size() == 2 && c2[0] == "src/main.cpp" && c2[1] == "src/mod.h");
    auto c3 = complete_path(root.string(), "read");
    CHECK(c3.size() == 1 && c3[0] == "README.md");                      // case-insensitive
    CHECK(complete_path(root.string(), ".h").size() == 1);
    CHECK(complete_path(root.string(), "nope/").empty());
    ctx.cwd = root.string();
    std::string text = "look at @src/ma now";
    m = compute_menu(text, 15, ctx);                                    // cursor after "@src/ma"
    REQUIRE(m.kind == Menu::At);
    CHECK(m.items[0].value == "@src/main.cpp");
    CHECK(text.substr(m.begin, m.end - m.begin) == "@src/ma");
    fs::remove_all(root);
    TEST_MAIN_END();
}
