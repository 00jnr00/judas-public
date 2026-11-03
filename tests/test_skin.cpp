// Skins: design tokens, CSS and JSON output, embedded built-in themes, user override by name.
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/skin.h"
#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

static bool has(const std::string & hay, const std::string & needle) { return hay.find(needle) != std::string::npos; }

int main() {
    // defaults
    Skin d;
    CHECK(d.token("radius") == "8px");
    CHECK(d.token("radius_sm") == "4px");
    CHECK(d.token("space") == "8px");
    CHECK(has(d.token("font_ui"), "system-ui"));
    CHECK(has(d.token("font_mono"), "ui-monospace"));
    CHECK(d.token("nope").empty());

    // parse: tokens override, unset ones keep defaults, colors merge over the defaults
    Skin p;
    CHECK(parse_skin_toml("name = \"t\"\nlabel = \"T\"\ndark = false\n[tokens]\nradius = '12px'\nfont_ui = 'Foo, \"Bar Baz\", sans-serif'\n[colors]\nbg = \"#ffffff\"\n", "fallback", p));
    CHECK(p.name == "t" && p.label == "T" && !p.dark);
    CHECK(p.token("radius") == "12px");
    CHECK(p.token("radius_sm") == "4px");
    CHECK(p.token("font_ui") == "Foo, \"Bar Baz\", sans-serif");
    CHECK(p.color("bg") == "#ffffff");
    CHECK(p.color("text") == d.color("text"));
    Skin q;
    CHECK(parse_skin_toml("dark = true\n", "fb", q) && q.name == "fb" && q.label == "fb");
    CHECK(!parse_skin_toml("name = [", "x", q));

    // css
    std::string css = skin_to_css(p);
    CHECK(has(css, "color-scheme: light;"));
    CHECK(has(css, "--bg: #ffffff;"));
    CHECK(has(css, "--radius: 12px;"));
    CHECK(has(css, "--radius-sm: 4px;"));
    CHECK(has(css, "--font-ui: Foo, \"Bar Baz\", sans-serif;"));
    CHECK(has(css, "--font-mono: "));
    CHECK(has(css, "--space: 8px;"));
    CHECK(has(css, "--accent-soft: rgba("));
    for (const char * k : {"bg", "surface", "surface2", "border", "title", "accent", "text", "dim", "ok", "warn", "error", "thinking", "added", "removed"})
        CHECK(has(css, std::string("--") + k + ": "));

    // json
    nlohmann::json j = skin_to_json(p);
    CHECK(j["name"] == "t" && j["label"] == "T" && j["dark"] == false);
    REQUIRE(j["vars"].is_object());
    CHECK(j["vars"]["--bg"] == "#ffffff");
    CHECK(j["vars"]["--radius"] == "12px");
    CHECK(j["vars"]["--radius-sm"] == "4px");
    CHECK(j["vars"]["--space"] == "8px");
    CHECK(j["vars"].contains("--font-ui") && j["vars"].contains("--font-mono"));
    CHECK(j["vars"].contains("--accent-soft"));
    CHECK(j["vars"].size() == skin_vars(p).size());

    // embedded built-ins exist without any user dir; judas stays first
    auto skins = load_skins({"/nonexistent-judas-skin-dir"});
    REQUIRE(!skins.empty());
    CHECK(skins[0].name == "judas");
    auto find = [&](const std::string & n) -> const Skin * {
        for (const auto & s : skins) if (s.name == n) return &s;
        return nullptr;
    };
    for (const char * n : {"iscariot", "work", "parchment"}) CHECK(find(n));
    for (const char * fam : {"ember-plum", "graphite", "glacier", "phosphor", "cinder", "tide"}) {
        const Skin * dk = find(std::string(fam) + "-dark");
        const Skin * lt = find(std::string(fam) + "-light");
        CHECK(dk && lt);
        if (dk) CHECK(dk->dark);
        if (lt) CHECK(!lt->dark);
    }
    CHECK(find("parchment") && !find("parchment")->dark);
    // theme names are plain words: lowercase letters and hyphens only (no initials, symbols or brands)
    for (const auto & s : skins) {
        bool plain = !s.name.empty();
        for (char c : s.name) plain = plain && ((c >= 'a' && c <= 'z') || c == '-');
        CHECK(plain);
    }

    // a user file beats an embedded theme of the same name and adds new ones
    fs::path dir = fs::path(JUDAS_TEST_TMP) / ("judas-skin-test-" + std::to_string(getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "graphite-dark.toml") << "name = \"graphite-dark\"\nlabel = \"Mine\"\n[tokens]\nradius = '1px'\n";
        std::ofstream(dir / "extra.toml") << "label = \"Extra\"\n";
        std::ofstream(dir / "broken.toml") << "name = [";
    }
    auto sk2 = load_skins({dir.string()});
    Skin g = skin_by_name("graphite-dark", {dir.string()});
    CHECK(g.label == "Mine" && g.token("radius") == "1px");
    int n = 0;
    for (const auto & s : sk2) if (s.name == "graphite-dark") n++;
    CHECK(n == 1);
    CHECK(skin_by_name("extra", {dir.string()}).label == "Extra");
    CHECK(skin_by_name("no-such-theme", {dir.string()}).name == "judas");
    fs::remove_all(dir);

    TEST_MAIN_END();
}
