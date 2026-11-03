// TUI themes: picker list, name resolution, the tui.toml preference, contrast, light themes stay foreground-only.
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "test.h"
#include "tui/theme.h"

namespace fs = std::filesystem;
using namespace judas;
using namespace judas::tui;

int main() {
    Skin a, b, c;
    CHECK(parse_skin_toml("name = \"graphite-dark\"\nlabel = \"Graphite Dark\"\ndark = true\n", "x", a));
    CHECK(parse_skin_toml("name = \"graphite-light\"\nlabel = \"Graphite Light\"\ndark = false\n[colors]\nbg = \"#f4f4f4\"\ntext = \"#202020\"\ndim = \"#909090\"\nadded = \"#c8f0c8\"\nremoved = \"#f0c8c8\"\n", "x", b));
    CHECK(parse_skin_toml("name = \"graphite-lime\"\ndark = true\n", "x", c));
    std::vector<Skin> skins = {a, b, c};

    auto list = list_themes(skins);
    REQUIRE(list.size() == 3);
    CHECK(list[0].name == "graphite-dark" && list[0].dark);
    CHECK(list[1].name == "graphite-light" && !list[1].dark && list[1].label == "Graphite Light");

    CHECK(resolve_theme(skins, "graphite-dark") == "graphite-dark");
    CHECK(resolve_theme(skins, "Graphite Light") == "graphite-light");   // by label, any case
    CHECK(resolve_theme(skins, "graphite-l") == "");                      // light and lime: ambiguous
    CHECK(resolve_theme(skins, "graphite-lig") == "graphite-light");
    CHECK(resolve_theme(skins, "nope") == "");
    CHECK(resolve_theme(skins, "") == "");

    // preference file
    CHECK(parse_theme_pref("") == "");
    CHECK(parse_theme_pref("theme = \"tide-dark\"\n") == "tide-dark");
    CHECK(parse_theme_pref("# c\n  theme='x'  # note\n") == "x");
    CHECK(parse_theme_pref("[ui]\ntheme = \"no\"\n") == "");              // only the top level is ours
    CHECK(with_theme_pref("", "a") == "theme = \"a\"\n");
    CHECK(with_theme_pref("theme = \"a\"\nother = 1\n", "b") == "theme = \"b\"\nother = 1\n");
    CHECK(with_theme_pref("other = 1\n", "b") == "other = 1\ntheme = \"b\"\n");
    CHECK(with_theme_pref("[ui]\nx = 1\n", "b") == "theme = \"b\"\n[ui]\nx = 1\n");
    CHECK(parse_theme_pref(with_theme_pref("[ui]\nx = 1\n", "b")) == "b");

    std::string dir = std::string(JUDAS_TEST_TMP) + "/tui-theme-" + std::to_string(getpid());
    fs::create_directories(dir);
    std::string path = dir + "/sub/tui.toml";
    CHECK(load_theme_pref(path) == "");
    CHECK(save_theme_pref(path, "tide-dark"));
    CHECK(load_theme_pref(path) == "tide-dark");
    { std::ofstream o(path, std::ios::app); o << "keep = true\n"; }
    CHECK(save_theme_pref(path, "cinder-light"));
    CHECK(load_theme_pref(path) == "cinder-light");
    { std::ifstream i(path); std::string all((std::istreambuf_iterator<char>(i)), {}); CHECK(all.find("keep = true") != std::string::npos); }
    fs::remove_all(dir);

    // contrast
    CHECK(luminance("#000000") < 0.001 && luminance("#ffffff") > 0.999);
    CHECK(luminance("zz") < 0);
    CHECK(contrast_ratio("#000000", "#ffffff") > 20.9);
    CHECK(ensure_contrast("#2f5a34", "#0b0b0d", 4.5) != "#2f5a34");       // dark green on near black gets lighter
    CHECK(contrast_ratio(ensure_contrast("#2f5a34", "#0b0b0d", 4.5), "#0b0b0d") >= 4.5);
    CHECK(ensure_contrast("#d7d7de", "#0b0b0d", 4.5) == "#d7d7de");       // already fine: untouched
    CHECK(contrast_ratio(ensure_contrast("#202020", "#000000", 4.5), "#000000") >= 4.5);   // dark text, dark terminal: lightened
    CHECK(contrast_ratio(ensure_contrast("#e0e0e0", "#ffffff", 4.5), "#ffffff") >= 4.5);  // pale text, light terminal: darkened

    CHECK(terminal_bg_from_colorfgbg("15;0") == "#000000");
    CHECK(terminal_bg_from_colorfgbg("0;15") == "#ffffff");
    CHECK(terminal_bg_from_colorfgbg("0;default;15") == "#ffffff");
    CHECK(terminal_bg_from_colorfgbg("0;default") == "");
    CHECK(terminal_bg_from_colorfgbg(nullptr) == "");

    // a look never paints a background (no 48;...) and keeps text readable
    auto no_bg = [](const ThemeLook & l) {
        for (const std::string * s : {&l.pal.dim, &l.pal.accent, &l.pal.text, &l.pal.ok, &l.pal.warn, &l.pal.err, &l.pal.think,
                                      &l.pal.code, &l.pal.user, &l.diff.add, &l.diff.del, &l.diff.hunk, &l.diff.head, &l.diff.ctx})
            if (s->find("48;") != std::string::npos || s->find("\033[7m") != std::string::npos) return false;
        return true;
    };
    ThemeLook dk = look_for(a, "");
    CHECK(no_bg(dk) && dk.warning.empty());
    CHECK(dk.pal.reset == "\033[0m" && dk.diff.add.find("38;2;") != std::string::npos);
    ThemeLook lt_on_dark = look_for(b, "#000000");
    CHECK(no_bg(lt_on_dark) && !lt_on_dark.warning.empty());
    ThemeLook lt_unknown = look_for(b, "");
    CHECK(no_bg(lt_unknown) && !lt_unknown.warning.empty());
    ThemeLook lt_on_light = look_for(b, "#ffffff");
    CHECK(no_bg(lt_on_light) && lt_on_light.warning.empty());
    CHECK(lt_on_dark.pal.text != lt_on_light.pal.text);                    // text adapts to the real terminal
    CHECK(look_for(b, "#000000").diff.add != look_for(b, "#ffffff").diff.add);

    TEST_MAIN_END();
}
