// Expert placement for MoE models that do not fit VRAM. The attention, norm and shared weights go to the GPU,
// the routed experts (ffn_*_exps) of the FIRST layers stay in RAM and the experts of the LAST layers go to
// the GPU as far as free VRAM allows (same shape as llama.cpp's --n-cpu-moe).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ggml.h"

namespace judas {

struct MoeLayout {
    bool ok = false;
    std::string error;
    int n_layers = 0;                      // block_count
    std::vector<uint64_t> expert_bytes;    // routed expert tensors per layer (all parts of a split file)
    uint64_t other_bytes = 0;              // everything else that goes to the GPU (attention, shared experts, output)
    uint64_t cpu_fixed_bytes = 0;          // input embedding tables: llama.cpp keeps these in RAM (huge per-layer tables on some models)
    uint64_t kv_bytes = 0;                 // KV cache for ctx_total at the chosen types (attention layers only)
    int attn_layers = 0;                   // layers that carry KV
    uint64_t expert_total() const;
};

// Reads tensor sizes and KV geometry from the GGUF header(s); no weights are loaded. Pass part 1 of a split.
MoeLayout read_moe_layout(const std::string & path, int ctx_total, ggml_type type_k, ggml_type type_v);

struct MoeFit {
    bool moe = false;            // has routed experts at all
    bool all_gpu = false;        // everything fits: no override needed
    int cpu_layers = 0;          // experts of layers [0, cpu_layers) stay on the CPU
    int gpu_expert_layers = 0;   // layers whose experts sit on the GPU
    uint64_t gpu_expert_bytes = 0, cpu_expert_bytes = 0;
    uint64_t gpu_total = 0;      // everything planned on the GPU incl. KV and compute buffers
    uint64_t budget = 0;         // VRAM left for experts after the fixed parts and the margin
    bool base_fits = true;       // false: even the non-expert weights + KV do not fit
};

constexpr uint64_t MOE_COMPUTE_DEFAULT = 1ull << 30;      // compute buffers estimate
constexpr uint64_t MOE_MARGIN_DEFAULT = 3ull << 29;       // 1.5 GiB safety margin

// Pure: decide how many leading layers keep their experts on the CPU.
MoeFit moe_fit(const MoeLayout & l, uint64_t free_vram, uint64_t margin = MOE_MARGIN_DEFAULT,
               uint64_t compute = MOE_COMPUTE_DEFAULT);

// Regex for llama's tensor_buft_overrides that matches the expert tensors of layers [0, n).
std::string moe_cpu_pattern(int n);

// What a cpu_moe config value asks for: -1 all layers on the CPU ("true"), -2 auto, 0 none ("false"),
// N the first N layers, -3 for bad text.
int moe_mode_value(const std::string & cpu_moe);

// Log line: "experts of layers 0..29 in RAM (25.1 GB), 30..47 on the GPU (10.3 GB); GPU total ...".
std::string moe_fit_summary(const MoeFit & f, int n_layers, uint64_t free_vram);

}  // namespace judas
