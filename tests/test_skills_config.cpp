// SkillsConfig: manage and track_usage parse, defaults, dump and default_config_toml.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/config.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static void write(const fs::path & p, const std::string & text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-skills-config-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root / "home");

    setenv("HOME", (root / "home").string().c_str(), 1);
    unsetenv("JUDAS_DATA_DIR");
    unsetenv("JUDAS_PORT");
    unsetenv("JUDAS_CONFIG");

    // Defaults.
    Config base = load_config((root / "empty-project").string());
    CHECK(base.skills.enabled);
    CHECK(base.skills.manage);
    CHECK(base.skills.track_usage);
    CHECK(base.skills.autoload == "trigger");

    // Explicit false values.
    fs::path cfg_file = root / "skills.toml";
    write(cfg_file, "[skills]\nenabled = true\nmanage = false\ntrack_usage = false\nautoload = \"off\"\n");
    setenv("JUDAS_CONFIG", cfg_file.string().c_str(), 1);
    Config c = load_config((root / "empty-project").string());
    CHECK(c.skills.enabled);
    CHECK(!c.skills.manage);
    CHECK(!c.skills.track_usage);
    CHECK(c.skills.autoload == "off");

    // dump_config keeps both fields and reloads to the same values.
    std::string dumped = dump_config(c);
    CHECK(dumped.find("manage = false") != std::string::npos);
    CHECK(dumped.find("track_usage = false") != std::string::npos);
    fs::path dump_file = root / "dump.toml";
    write(dump_file, dumped);
    setenv("JUDAS_CONFIG", dump_file.string().c_str(), 1);
    Config again = load_config((root / "empty-project").string());
    CHECK(!again.skills.manage);
    CHECK(!again.skills.track_usage);

    // The generated default config documents both switches.
    std::string defaults = default_config_toml();
    CHECK(defaults.find("manage = true") != std::string::npos);
    CHECK(defaults.find("track_usage = true") != std::string::npos);

    unsetenv("JUDAS_CONFIG");
    fs::remove_all(root);
    TEST_MAIN_END();
}
