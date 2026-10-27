// LearningConfig ([learning]): curator keys parse, dump, and appear in the init template.
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/config.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static void write(const fs::path & p, const std::string & text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-curator-config-" + std::to_string(getpid());
    fs::remove_all(root);

    setenv("HOME", (root / "home").string().c_str(), 1);
    unsetenv("JUDAS_DATA_DIR");
    unsetenv("JUDAS_PORT");
    unsetenv("JUDAS_CONFIG");

    // Defaults per the contract.
    Config base = load_config((root / "project").string());
    CHECK(base.learning.curator);
    CHECK(base.learning.curator_interval_hours == 168);
    CHECK(base.learning.curator_min_idle_minutes == 30);
    CHECK(base.learning.stale_after_days == 30);
    CHECK(base.learning.archive_after_days == 90);
    CHECK(base.learning.curator_backups == 5);

    // Explicit values, including curator off.
    fs::path cfg_file = root / "c.toml";
    write(cfg_file, "[learning]\ncurator = false\n"
                    "curator_interval_hours = 24\n"
                    "curator_min_idle_minutes = 5\n"
                    "stale_after_days = 7\n"
                    "archive_after_days = 60\n"
                    "curator_backups = 2\n");
    setenv("JUDAS_CONFIG", cfg_file.string().c_str(), 1);
    Config c = load_config((root / "project").string());
    CHECK(!c.learning.curator);
    CHECK(c.learning.curator_interval_hours == 24);
    CHECK(c.learning.curator_min_idle_minutes == 5);
    CHECK(c.learning.stale_after_days == 7);
    CHECK(c.learning.archive_after_days == 60);
    CHECK(c.learning.curator_backups == 2);

    // dump_config keeps every key, and the dump reloads to the same values.
    std::string dumped = dump_config(c);
    CHECK(dumped.find("[learning]") != std::string::npos);
    CHECK(dumped.find("curator = false") != std::string::npos);
    CHECK(dumped.find("curator_interval_hours = 24") != std::string::npos);
    CHECK(dumped.find("curator_min_idle_minutes = 5") != std::string::npos);
    CHECK(dumped.find("stale_after_days = 7") != std::string::npos);
    CHECK(dumped.find("archive_after_days = 60") != std::string::npos);
    CHECK(dumped.find("curator_backups = 2") != std::string::npos);
    fs::path dump_file = root / "dump.toml";
    write(dump_file, dumped);
    setenv("JUDAS_CONFIG", dump_file.string().c_str(), 1);
    Config again = load_config((root / "project").string());
    CHECK(!again.learning.curator && again.learning.curator_backups == 2);

    // Problems that are not parse errors (other validators may also fire; this file only checks its own).
    write(cfg_file, "[learning]\ncurator_interval_hours = 0\n");
    setenv("JUDAS_CONFIG", cfg_file.string().c_str(), 1);
    std::vector<std::string> problems = validate_config(load_config((root / "project").string()));
    CHECK(std::any_of(problems.begin(), problems.end(),
                      [](const std::string & p) { return p.find("curator_interval_hours") != std::string::npos; }));

    write(cfg_file, "[learning]\nstale_after_days = 30\narchive_after_days = 10\n");
    setenv("JUDAS_CONFIG", cfg_file.string().c_str(), 1);
    problems = validate_config(load_config((root / "project").string()));
    CHECK(std::any_of(problems.begin(), problems.end(),
                      [](const std::string & p) { return p.find("archive_after_days") != std::string::npos; }));

    // The generated default config documents the section.
    std::string defaults = default_config_toml();
    CHECK(defaults.find("[learning]") != std::string::npos);
    CHECK(defaults.find("curator_interval_hours = 168") != std::string::npos);
    CHECK(defaults.find("archive_after_days = 90") != std::string::npos);

    unsetenv("JUDAS_CONFIG");
    fs::remove_all(root);
    TEST_MAIN_END();
}
