# Expert cache: putting the hot experts in VRAM

Status: design only. What exists today is layer-granular placement (`cpu_moe = "auto"`, see README). This note
covers going finer: placing individual experts, the way Strata does.

## Where we are

llama.cpp places whole tensors. A layer's `ffn_{up,gate,down}_exps` is one tensor holding all experts of that
layer, so a buffer type override moves all 512 experts of layer N or none. `cpu_moe = "auto"` therefore keeps the
experts of the last K layers on the GPU and the first layers' experts in RAM (Qwen3.8 IQ2_XS on 16 GB: 12 of 48
layers, 9.1 GB of 35.5 GB of experts on the GPU, 26.3 GB in RAM). Every token still routes through all 48 layers,
and in 36 of them the CPU computes the chosen experts.

## What Strata does (docs/DETAILS.md, "How it works"; src/core/expert_cache.cpp)

- All experts live in pinned RAM. The VRAM left after attention, routers, shared experts, KV and buffers is an
  **expert cache** of per-expert slots, filled from a routing profile (most used experts first).
- A token's chosen experts that are in VRAM run on the GPU; the misses run on the CPU in place, at the same
  time (a share of misses can also go over PCIe, `--pcie-frac`).
- The cache adapts while you chat (`--adapt-every`): a swap copies the evicted expert back into the RAM slot of
  the one that replaces it.
- Prompt processing streams a layer's experts over PCIe while the previous layer computes.
- Extras: RAM budget tiers, mmap of the GGUF, next-layer router lookahead.

The gain comes from routing skew: a small fraction of experts takes most of the tokens, so the same VRAM covers
many more tokens' work when it holds hot experts instead of whole layers.

## Why it is not a config change in llama.cpp

`ggml_mul_mat_id(w, x, ids)` takes one weight tensor `w[ne0, ne1, n_expert]` and expert ids into it. Both pieces
of data (which backend holds `w`, which experts exist in it) are fixed at graph build. The scheduler
(`ggml_backend_sched`) assigns each op to the backend of its weights, runs splits one after another, and copies
inputs between them. Nothing lets one op use two memory pools. The relevant code: `build_moe_ffn` in
`src/llama-graph.cpp` (per architecture), the buft override matching at `src/llama-model-loader.cpp:1235`, and the
scheduler in `ggml/src/ggml-backend.cpp`.

## Options

### A. Static hot/cold split at load (recommended first step)

At load, per layer, build two tensors from the one in the file: `hot` (k experts, in a GPU buffer) and `cold`
(the other E-k, in a CPU buffer), plus a per-layer map `expert id -> (hot?, slot)`.
Graph: after the router's top-k, compute `ids_hot` and `ids_cold` with a gather through the map (new small ggml
ops or computed on the CPU as the ids are tiny), run `mul_mat_id` on `hot` with the hot ids and on `cold` with the
cold ids, and combine. Unused slots are masked by giving them weight 0 in the routed weight tensor, so the
output is exactly the same sum.
- The hot set comes from a calibration run: a routing profile (per layer, per expert counts) collected by a
  small callback in `build_moe_ffn` or via `llama_set_eval_callback` on `ffn_moe_topk` tensors, stored as JSON
  next to the GGUF (`<model>.routing.json`). Strata ships profiles; ours would be measured on the user's own prompts
  (agent/coding traffic), which is closer to real use than a generic profile.
- Loader work: a new override path that, for tensors named `*_exps`, slices along the expert dimension (the
  expert axis is the slowest axis in the file, so a slice is a contiguous byte range per expert: copy rows
  directly from the mmap, no repacking).
- Effort: about 2 to 3 weeks of focused work. 1 week loader + slicing + profile format; 1 week graph changes for
  the architectures we use (qwen35moe / qwen4exp, gemma4, gpt-oss; each has its own `build_moe_ffn` call or
  variant); 1 week correctness tests (bit-identical logits vs unsplit, perplexity) and tuning.
- Risk: it is a patch against llama.cpp internals that changes between pulls (we already carry the logit-bias
  patch). Keep it small and in one file where possible, or contribute upstream.
- Gain (estimate, to be measured): same VRAM as the layer split (9 GB of experts, about 26 percent of the expert
  bytes). If the top 26 percent of experts take 50 to 60 percent of the routings (typical skew; Strata's profile
  data gives the real number), the CPU's expert work per token falls from about 75 percent to 40 to 50 percent,
  so roughly 1.4x to 1.8x on the CPU-bound part of decode. The split adds two sequential mul_mat_id launches per
  layer instead of one and one more small sync, which costs part of that; prompt processing gains less (large
  batches route to nearly every expert).
- Hybrid: the layer split of `auto` stays as the fallback and for models without a profile.

### B. VRAM slot cache with runtime remap (Strata-style, adaptive)

Keep all experts in RAM (pinned), a fixed VRAM tensor `slots[ne0, ne1, S]` per layer or one pool for all layers,
and a remap table `expert id -> slot or -1` updated at runtime. Per token: ids through the remap, hits run on
the GPU against `slots`, misses on the CPU against the RAM tensor; a background thread swaps experts using the
running routing counts.
- Needs everything in A plus: a mutable device tensor updated between graph evaluations (host-to-device copies
  on a copy stream, synchronization with the decode graph), a stats collector, an eviction policy, and a CPU
  split that reads the RAM tensor directly. The graph is built once (n_expert slots fixed), only the remap and
  slot contents change, so no graph rebuild.
- Effort: 5 to 8 weeks on top of A (the concurrency and correctness work is the cost). Strata took a whole
  engine to do this well.
- Gain: over A, mostly adapting to the conversation (code vs prose routes differently) and global rather than
  per-layer budgeting of slots. Likely another 10 to 30 percent over a good static profile; measure A first to see
  how much a perfect per-prompt oracle would add before building this.

### C. Overlap CPU and GPU

In A and B the hot and cold ops of a layer run one after the other. Running them in parallel (GPU hot while the
CPU does cold) needs an async split in the scheduler or a custom op that launches the GPU part and returns before
it finishes. Potential: up to 2x on the layer's expert time when both halves are similar. Effort 2 to 4 weeks and
fragile; skip until A is measured.

### D. Use Strata itself for models that need it

Strata already does B for the Qwen3.8 family (RAM-pinned experts, adaptive cache, MTP speculation) at about
60 to 90 tokens/s on a 12 GB card. For that model the shortest path to speed is running Strata (:8081 remote
model in Judas), not reimplementing it. The llama.cpp work in A is for the models Strata does not cover (Gemma 4
26B, GPT-OSS, Ornith, other MoE GGUFs) and for keeping one harness with in-process prefix sharing.

## Recommended phases

0. Done: `cpu_moe = "auto"` layer split (this branch). Measure it on the real model first (decode and prefill
   tok/s vs all-experts-on-CPU) when the GPU is free; that is the baseline every later step must beat.
1. Routing profiler (2 to 3 days): an eval callback that records top-k ids per layer for a prompt set, plus
   `judas models profile <model> <prompts>`. Output: JSON of counts. No change to inference. This alone answers
   how skewed routing is for our models and what hit rate a given VRAM budget buys; if the top 26 percent of
   experts cover under 40 percent of routings, stop here.
2. Static hot set (option A), for one architecture first (Qwen3.8 / qwen4exp since we have the file and Strata
   numbers to compare), correctness tests against the unsplit model, then the other architectures.
3. Decide on B and C using the measured gap between the static set and a per-prompt oracle (computed from the
   profiler traces, no new engine code).

## Things to carry into any of these

- The "refuse if more than 5 GB VRAM is in use" guard and the fit math (`src/models/moefit.cpp`) stay the budget
  source; the hot-set size is `budget / bytes per expert`, not a layer count.
- RAM: on Qwen3.8 the input tables (`per_layer_token_embd`, 28.8 GB) already stay in RAM; with 26 GB of experts
  that is 55 GB of a 64 GB box. mmap-backed weights let the OS drop pages, but decode then reads from SSD.
  Keep an eye on this when giving more layers to the CPU.
- No experts of two different models in VRAM at once (Judas runs one model at a time).
