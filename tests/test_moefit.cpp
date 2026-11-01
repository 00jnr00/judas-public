// MoE expert placement: pure fit math, cpu_moe value parsing, and (when the file is on disk) the real Qwen3.8 numbers.
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>

#include "core/config.h"
#include "models/moefit.h"
#include "test.h"
#include <unistd.h>

using namespace judas;
namespace fs = std::filesystem;
static constexpr uint64_t GB = 1000ull * 1000 * 1000;
static constexpr uint64_t GiB = 1ull << 30;

int main() {
    CHECK(moe_mode_value("false") == 0 && moe_mode_value("true") == -1 && moe_mode_value("auto") == -2);
    CHECK(moe_mode_value("12") == 12 && moe_mode_value("x") == -3 && moe_mode_value("-4") == -3);
    CHECK(moe_cpu_pattern(3) == "blk\\.(0|1|2)\\.ffn_(up|down|gate)_exps");

    // 10 layers x 4 GB of experts, 2 GB other, 1 GB KV
    MoeLayout l;
    l.ok = true;
    l.n_layers = 10;
    l.expert_bytes.assign(10, 4 * GB);
    l.other_bytes = 2 * GB;
    l.kv_bytes = 1 * GB;
    // free 20 GiB: fixed = 2+1 GB + 1 GiB compute + 1.5 GiB margin; budget ~ 21.47 - 3 - 2.68 = 15.8 GB -> 3 layers
    MoeFit f = moe_fit(l, 20 * GiB);
    CHECK(f.moe && !f.all_gpu && f.gpu_expert_layers == 3 && f.cpu_layers == 7);
    CHECK(f.gpu_expert_bytes == 12 * GB && f.cpu_expert_bytes == 28 * GB);
    CHECK(f.gpu_total <= 20 * GiB - MOE_MARGIN_DEFAULT);
    // plenty of VRAM: everything on the GPU, no override
    f = moe_fit(l, 80 * GiB);
    CHECK(f.all_gpu && f.cpu_layers == 0);
    // too little for the base weights: all experts in RAM and a warning
    f = moe_fit(l, 3 * GiB);
    CHECK(!f.base_fits && f.cpu_layers == 10 && f.gpu_expert_layers == 0);
    // dense model: nothing to place
    MoeLayout d = l;
    d.expert_bytes.assign(10, 0);
    CHECK(!moe_fit(d, 4 * GiB).moe);
    // uneven layers: the suffix stops at the first layer that does not fit
    MoeLayout u = l;
    u.expert_bytes = {1 * GB, 1 * GB, 9 * GB, 1 * GB, 1 * GB};
    u.n_layers = 5;
    f = moe_fit(u, 10 * GiB, 0, 0);   // budget = 10.74 - 3 = 7.7 GB: layers 4,3 fit, layer 2 (9 GB) stops it
    CHECK(f.gpu_expert_layers == 2 && f.cpu_layers == 3);
    CHECK(moe_fit_summary(f, 5, 10 * GiB).find("layers 0..2 in RAM") != std::string::npos);

    // config: true / false / N / "auto" round trip through the loader and the writer
    fs::path root = fs::path(JUDAS_TEST_TMP) / ("moefit-" + std::to_string(getpid()));
    fs::create_directories(root);
    setenv("JUDAS_CONFIG", (root / "c.toml").c_str(), 1);
    for (const char * v : {"true", "false", "7", "\"auto\"", "\"12\""}) {
        std::ofstream(root / "c.toml")
            << "[engine]\nmodel = \"m\"\n[models.m]\npath = \"/x.gguf\"\ncpu_moe = " << v << "\n";
        Config c = load_config(root.string());
        std::string want = v;
        want.erase(std::remove(want.begin(), want.end(), '"'), want.end());
        CHECK(c.models["m"].cpu_moe == want);
    }
    std::ofstream(root / "c.toml") << "[engine]\nmodel = \"m\"\n[models.m]\npath = \"/x.gguf\"\n";
    CHECK(load_config(root.string()).models["m"].cpu_moe == "auto");   // default is GPU-first
    std::ofstream(root / "c.toml") << "[engine]\nmodel = \"m\"\n[models.m]\npath = \"/x.gguf\"\ncpu_moe = \"maybe\"\n";
    bool threw = false;
    try { load_config(root.string()); } catch (...) { threw = true; }
    CHECK(threw);
    unsetenv("JUDAS_CONFIG");
    fs::remove_all(root);

    // the real thing, if the file is here (reads headers only, loads no weights)
    const char * env = std::getenv("JUDAS_TEST_QWEN38_GGUF");   // optional: a Qwen3.8-Flash-Next IQ2_XS part 1
    const char * q = env ? env : "/nonexistent";
    if (fs::exists(q)) {
        MoeLayout r = read_moe_layout(q, 49152, GGML_TYPE_Q8_0, GGML_TYPE_Q8_0);
        CHECK(r.ok && r.n_layers == 48 && r.expert_bytes.size() == 48);
        CHECK(r.expert_total() > 35 * GB && r.expert_total() < 36 * GB);
        CHECK(r.other_bytes > 3 * GB && r.other_bytes < 4500ull * 1000 * 1000 && r.cpu_fixed_bytes > 28 * GB);
        CHECK(r.attn_layers == 12);
        for (uint64_t vram : {(uint64_t) 16 * GB, (uint64_t) 16 * GiB}) {
            MoeFit g = moe_fit(r, vram);
            std::printf("qwen3.8 %.1f GB VRAM: other %.2f GB, kv %.2f GB, %s\n", (double) vram / 1e9,
                        (double) r.other_bytes / 1e9, (double) r.kv_bytes / 1e9, moe_fit_summary(g, r.n_layers, vram).c_str());
            CHECK(!g.all_gpu && g.cpu_layers > 0 && g.cpu_layers < 48);
            CHECK(g.gpu_total + MOE_MARGIN_DEFAULT <= vram);
        }
        MoeFit g = moe_fit(r, 11 * GiB);   // what is left with 5 GB already taken
        std::printf("qwen3.8 11 GiB free: %s\n", moe_fit_summary(g, r.n_layers, 11 * GiB).c_str());
    }
    TEST_MAIN_END();
}
