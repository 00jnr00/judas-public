// Engine prefill fairness config: defaults, parsing, dump round-trip and validation, no model or GPU.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

#include "core/config.h"
#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

static void write(const fs::path & p, const std::string & text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

static bool mentions(const std::vector<std::string> & errors, const std::string & key) {
    for (const auto & e : errors)
        if (e.find(key) != std::string::npos) return true;
    return false;
}

int main() {
    fs::path root = fs::temp_directory_path() / ("judas-engine-sched-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "home");
    setenv("HOME", (root / "home").c_str(), 1);
    setenv("JUDAS_CONFIG", (root / "cfg.toml").c_str(), 1);
    unsetenv("JUDAS_DATA_DIR");
    unsetenv("JUDAS_PORT");

    // Defaults keep today's behavior: full duty, busy chunk follows prefill_chunk, 2 s guard.
    Config c = load_config(root);
    CHECK(c.engine.prefill_duty == 1.0);
    CHECK(c.engine.prefill_chunk_busy == 0);
    CHECK(c.engine.busy_prefill_chunk() == c.engine.prefill_chunk);
    CHECK(c.engine.prefill_max_wait_ms == 2000);

    // Explicit values are parsed.
    write(root / "cfg.toml",
          "[engine]\nprefill_duty = 0.25\nprefill_chunk_busy = 64\nprefill_max_wait_ms = 500\n");
    c = load_config(root);
    CHECK(c.engine.prefill_duty == 0.25);
    CHECK(c.engine.busy_prefill_chunk() == 64);
    CHECK(c.engine.prefill_max_wait_ms == 500);

    // An unset busy chunk follows a user-changed prefill_chunk.
    write(root / "cfg.toml", "[engine]\nprefill_chunk = 512\n");
    c = load_config(root);
    CHECK(c.engine.busy_prefill_chunk() == 512);

    // Dump and load round-trip the knobs.
    c.engine.prefill_duty = 0.5;
    c.engine.prefill_chunk_busy = 96;
    c.engine.prefill_max_wait_ms = 750;
    write(root / "m.gguf", "x");
    c.models["big"].path = "~/m.gguf";
    write(root / "dump.toml", dump_config(c));
    setenv("JUDAS_CONFIG", (root / "dump.toml").c_str(), 1);
    Config d = load_config(root);
    CHECK(d.engine.prefill_duty == 0.5);
    CHECK(d.engine.busy_prefill_chunk() == 96);
    CHECK(d.engine.prefill_max_wait_ms == 750);

    // Validation rejects values outside the documented ranges.
    Config b = c;
    b.engine.prefill_duty = 0.0;
    CHECK(mentions(validate_config(b), "prefill_duty"));
    b.engine.prefill_duty = 1.5;
    CHECK(mentions(validate_config(b), "prefill_duty"));
    b = c;
    b.engine.prefill_chunk_busy = -1;
    CHECK(mentions(validate_config(b), "prefill_chunk_busy"));
    b = c;
    b.engine.prefill_max_wait_ms = -1;
    CHECK(mentions(validate_config(b), "prefill_max_wait_ms"));
    b = c;
    b.engine.prefill_duty = 1.0;
    b.engine.prefill_chunk_busy = 0;
    CHECK(!mentions(validate_config(b), "prefill_"));

    fs::remove_all(root, ec);
    TEST_MAIN_END();
}
