// Pure mapping logic of the Strata backend (no GPU, no process, no llama.cpp): Judas settings -> Strata's command
// line and request lines, Strata's output lines -> events. Always compiled, unit-tested in test_strata_proto.cpp.
// Protocol reference: ~/src/strata src/program/generate.cpp, the "--serve" block (GEN / T / PP / RESUME / DONE / ERR).
#pragma once

#include <set>
#include <string>
#include <vector>

#include "core/config.h"

namespace judas {

// What Strata's sampler accepts per request. Anything absent keeps Strata's own default (greedy, no penalties).
struct StrataSampling {
    float temp = 0, top_p = 1, min_p = 0;
    int top_k = 20;
    unsigned long long seed = 0;   // 0 = random
};

// The values for one request: request overrides (< 0 = unset) over the model's [models.x.sampling] defaults.
StrataSampling strata_sampling(float req_temp, float req_top_p, float req_min_p, int req_top_k, unsigned req_seed,
                               const ModelConfig & m);

// "GEN <max_new> key=value ... id,id,id" (no trailing newline). Mirrors Strata's own serve/server.py
// sampling_keys(): temperature only when > 0, top_p only when < 1, top_k clamped to 1..64 (the sampled path keeps
// at most 64 candidates), min_p only in (0, 1], seed only when > 0.
std::string strata_gen_line(int max_new, const StrataSampling & s, const std::vector<int> & ids);

// Strata splits a model in two GGUF files (the transformer, then the per-layer-embedding table); --native takes the
// first and --ple-gguf the second. Returns "" when `path` is not a "-0000K-of-0000N.gguf" shard name or N != 2.
std::string strata_second_shard(const std::string & path);

// Directory of the pack's tokenizer files, "" when no pack is set.
std::string strata_chat_template_path(const ModelConfig & m);

// The engine arguments (after the executable; judas starts itself as `judas __strata-engine <these>`). Defaults reproduce the 47 tok/s IQ2_XS setup measured on
// the RX 6800 (strata-iq2_xs.json): spec 4, spec-min-p 0.5, expert cache auto, prefill auto, int8 KV, 3072 MiB
// VRAM reserve. `max_context` is the model's ctx_total (one sequence gets all of it). Throws std::runtime_error
// naming the missing setting when the config cannot start the engine (pack and mtp are required).
// `profile` is the resolved expert profile path (strata_profile, else the extracted embedded copy).
std::vector<std::string> strata_argv(const ModelConfig & m, int max_context, const std::string & profile);

// One line of engine output.
struct StrataLine {
    enum Kind { Token, Prompt, Resume, Reused, Done, Err, Info, Ready, Other } kind = Other;
    int token = 0;                         // Token
    int pos = 0, total = 0;                // Prompt: tokens read so far / prompt length
    double tok_s = 0;                      // Prompt: prefill rate
    int reused = 0;                        // Resume / Reused: prompt tokens not read again
    // Done
    int generated = 0, prompt_tokens = 0;
    double prompt_ms = 0, decode_ms = 0;
    std::string finish;                    // stop | length | cancel
    int drafts_accepted = -1, drafts_offered = -1;
    int prompt_read = -1;                  // tokens actually read (engine 0.1.36+), -1 when absent
    std::string text;                      // Err: the message; Ready: raw line
    int max_context = 0;                   // Ready
    bool can_stop = false;                 // Ready: the engine understands STOP
};
StrataLine parse_strata_line(const std::string & line);

// Map Strata's finish word onto Judas's finish_reason (stop | length | cancelled).
std::string strata_finish_reason(const std::string & strata_finish);

// Length of the longest prefix of `s` that does not end inside a UTF-8 sequence. Token pieces can split a
// multi-byte character; the tail is held back until its remaining bytes arrive.
size_t utf8_complete_prefix(const std::string & s);

}  // namespace judas
