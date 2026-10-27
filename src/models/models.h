// Finding and connecting models: scan the usual places for GGUF files (your model dirs, LM Studio, the
// Hugging Face cache, Ollama's blob store), read their metadata, judge whether they fit the GPU, search and
// download from Hugging Face, and write a [models.<name>] block into the config.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/config.h"

namespace judas {

struct GgufInfo {
    std::string path;          // first file (split models: part 1)
    std::string source;        // "dir", "lm-studio", "huggingface", "ollama", "config"
    std::string label;         // what to call it in a list (file name, or ollama name:tag)
    uint64_t bytes = 0;        // all parts together
    int parts = 1;
    bool ok = false;           // header parsed
    std::string error;
    std::string arch, name, size_label, quant;
    uint32_t ctx_train = 0;
    uint32_t experts = 0, experts_used = 0;
    bool projector = false;    // a vision projector (mmproj), not a model you can load alone
    bool helper = false;       // embedding, reranker or MTP draft head: loadable, but not a chat model
    std::string config_name;   // [models.<x>] that points at this file, if any
};

// Read one GGUF header (no tensor data). Works for split files (pass part 1).
GgufInfo gguf_info(const std::string & path);

// Directories scanned by default, in order, plus JUDAS_MODEL_DIRS (colon separated) and the folders of
// every configured model. Missing ones are skipped.
std::vector<std::pair<std::string, std::string>> model_search_dirs(const Config & cfg);

// Every model found, sorted by source then path, deduplicated by real path; configured models first.
std::vector<GgufInfo> scan_models(const Config & cfg, const std::vector<std::string> & extra_dirs = {});

// Total VRAM of the biggest GPU in bytes (AMD sysfs, else nvidia-smi), 0 when unknown.
uint64_t gpu_vram_total();

// "fits" | "tight" | "too big" (for offloading the whole model) given the file size and VRAM.
std::string fit_verdict(uint64_t model_bytes, uint64_t vram);

// A config-safe name from a file name: "Qwen3-30B-A3B-Q4_K_M.gguf" -> "qwen3-30b-a3b".
std::string suggest_model_name(const std::string & file_name);

// The TOML block for a model with defaults that suit the file and the GPU (cpu: gpu_layers = 0, RAM-sized pool).
std::string model_block(const std::string & name, const GgufInfo & m, uint64_t vram, bool cpu = false);

// Append a [models.<name>] block to the config file (created with defaults if missing). Error text or "".
std::string add_model_to_config(const std::string & config_path, const std::string & name, const GgufInfo & m,
                                uint64_t vram, bool cpu = false);

// Hugging Face (needs network). HF_TOKEN is sent when set (gated repos).
struct HfRepo { std::string id; long long downloads = 0; long long likes = 0; };
struct HfFile { std::string path; uint64_t bytes = 0; };
std::vector<HfRepo> hf_search(const std::string & query, int limit, std::string * err);
std::vector<HfFile> hf_gguf_files(const std::string & repo, std::string * err);
// Download repo/file into dir (resumes a .part). progress(done, total). Returns the local path or "" (+err).
std::string hf_download(const std::string & repo, const std::string & file, const std::string & dir,
                        const std::function<void(uint64_t, uint64_t)> & progress, std::string * err);

std::string human_bytes(uint64_t b);

// The stock name of a GGUF file: its file name without .gguf or a split suffix ("X-00001-of-00002.gguf" -> "X").
std::string gguf_display_name(const std::string & path_or_label);

}  // namespace judas
