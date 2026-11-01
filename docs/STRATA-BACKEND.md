# Strata backend

Judas can run Qwen3.8-Flash-Next on the Strata engine (github.com/Niko1221/Strata, MIT, Copyright Niko1221 and
the Strata contributors) instead of llama.cpp. Strata keeps the experts of a mixture-of-experts model in
per-expert VRAM slots (adaptive), the rest in RAM, and drafts with the model's own MTP head. On the RX 6800
16 GB (gfx1030) Strata's own server measured about 47 tok/s with the IQ2_XS model.

## Design choice: Strata compiled into judas, run as a child process

Strata's static libraries have no engine entry point: model loading, the expert cache plan, prefill lending, MTP
drafting and the request loop are all inside `main()` of `src/program/generate.cpp` (6.9k lines, the `--serve`
block) on that process's stdin and stdout. Strata's own Python server drives it by starting `strata --serve` and
speaking a line protocol (below), and unloading must give the GPU back, which only process exit does reliably
(Strata frees tens of GB of pinned RAM and VRAM there; Judas's model switcher depends on it).

So with `-DJUDAS_STRATA=ON` the engine is built into the judas binary and still runs in its own process:

- `cmake/strata.cmake` adds `~/src/strata` as a subdirectory (read only, never edited) with its HIP backend, and
  compiles `generate.cpp` once more into `judas_strata_main` with `-Dmain=strata_main`. The libraries
  (`strata_engine`, `strata_prefill`, `strata_spec`, kernels, and Strata's pinned ggml) link into judas.
- `judas __strata-engine <strata args>` (hidden, `src/main.cpp`) calls `strata_main`.
- `StrataEngine` starts `/proc/self/exe __strata-engine --serve ...` with pipes. No separate Strata binary, no
  Python, no checkout of Strata at run time. The child dies with Judas (`PR_SET_PDEATHSIG`), is asked to `QUIT`
  on unload, then terminated, then killed.

Build time needs ROCm (`/opt/rocm`: hipcc/clang++, hipblas) and the Strata sources; OFF needs neither. The binary
then links the ROCm runtime dynamically (`libamdhip64`, `libhipblas`, found through an rpath to `/opt/rocm/lib`),
like any HIP program; that is a system dependency, not a Strata one.

Runtime data files Strata needs:

- expert profile (`data/expert-profile.bin`, 190 KB): embedded in the binary; written once to
  `<data_dir>/strata/expert-profile.bin` for the engine to read. `strata_profile` overrides it.
- chat template: taken from the model GGUF itself (it carries `tokenizer.chat_template`); `chat_template_file`
  overrides it, and the pack's `tokenizer/chat_template.jinja` is the last fallback.
- tokenizer: from the model GGUF (below). GPU kernels are compiled into the binary (no separate code objects).
- model files, `strata_pack` (dense weights, index) and `strata_mtp`: these are the user's model data and stay where
  they are, as the llama backend's GGUF does.

Symbol collision: Strata statically links its own ggml (0.24.0, the same line as llama.cpp's shared libggml).
The link uses `-Wl,--exclude-libs,ALL` so the static copies are not exported and libllama keeps binding to its own
libggml (checked: no `ggml_`/`gguf_` in the dynamic table). Strata's ggml and Judas's are different builds of
nearly the same source; the GPU run is the real test of this.

## What Judas does and what Strata does

| Judas | Strata |
| --- | --- |
| chat template (pack `tokenizer/chat_template.jinja`, rendered by llama.cpp's jinja) | weights, expert cache, KV |
| tokenizer (see below) | prefill, decode, MTP and suffix drafting |
| sampling parameters sent per request | sampling itself, stop on `max_new` or STOP |
| output parsing: reasoning, tool calls, incremental deltas | conversation checkpoints (prefix reuse) |
| queueing agents, priorities, cancel | |

### Protocol (Strata `--serve`)

Startup prints `INFO k=v ...` lines, then `READY <max_context> [stop]`. A request is one line,
`GEN <max_new> [temperature=F] [top_p=F] [top_k=N] [min_p=F] [seed=N] <id,id,...>`; the answer is `RESUME n` /
`REUSED n` (prompt tokens not read again), `PP <pos> <total> <ms> <tok/s>` per prompt chunk, `T <id>` per token,
then `DONE <generated> <prompt_tokens> <prompt_ms> <decode_ms> <stop|length|cancel> ...` or `ERR <msg>`. `STOP`
ends the running request at its next step (the engine still sends `DONE`), `QUIT` ends the process. The pure
mapping of this (request line, output line parsing, command line, config) is `src/engine/strata_proto.{h,cpp}`
and is unit-tested in `tests/test_strata_proto.cpp`.

### Tokenizer

Strata ships its tokenizer as Python. Its GGUF architecture (`qwen4exp`) is unknown to libllama, but the
vocabulary is byte-level BPE with the `qwen35` pre-tokenizer. Judas copies the `tokenizer.ggml.*` keys of the
model into a tiny synthetic GGUF that claims arch `qwen35` and loads it `vocab_only` (no tensors, no GPU, a
second of CPU). The ids match Strata's reference tokenizer (`tools/strata_tokenizer.py`) on the strings in
`tests/strata_tokenizer_check.cpp`. End-of-turn detection is libllama's EOG set (`<|im_end|>`, `<|endoftext|>`).

### Requests and concurrency

Strata serves one sequence at a time. Judas puts agents in a `TicketQueue` (foreground first, FIFO within a
priority, the same queue the remote backend uses) and sends one `GEN` at a time; each agent sends its whole prompt
and Strata continues from the live session or the longest conversation checkpoint it holds
(`--prompt-cache`, default 6), so a lone agent keeps prefix reuse. Cancel (`on_delta` returns false, or the
cancel flag) sends `STOP` and drains to `DONE`. The context is `ctx_total` for the one running request
(`--max-context`); `ctx_per_agent` is not used by the engine.

## What degrades against llama.cpp

- No multi-agent batching: with N agents the aggregate stays at one stream (about 47 tok/s total), where
  llama.cpp batches 8 agents at about 260 tok/s. Agents take turns; a long prompt from one holds up the rest.
- Prefix sharing between agents is gone (no shared system-prompt snapshot); only Strata's checkpoints, which
  help when the same conversation comes back. With several agents interleaving, each switch can re-read the
  prompt (Strata reads about 1100 tok/s on this GPU, so a 20k prompt costs about 18 s).
- No vision (Strata needs its own `strata-vision` encoder and embedding files; `vision()` is false).
- No logit bias: `logit_bias` and `bias_file` (the em dash ban) are ignored, with a warning at start.
- `top_k` is capped at 64 by the sampled path; `repeat_penalty` and the other penalties are not forwarded.
- Context is bounded by Strata's `--max-context` and KV (int8, 32768 resident tokens by default), not by the
  llama.cpp KV pool; `/api/state` reports one sequence and VRAM from sysfs.
- Hang detection is by process exit only (Strata's own server adds a silence watchdog; not ported).

## Config

```toml
[engine]
model = "titan"

[models.titan]
backend = "strata"                      # default "llama"
path = "~/models/strata/IQ2_XS/Qwen3.8-Flash-Next-GSQ-RCO-IQ2_XS-00001-of-00002.gguf"   # shard 1; shard 2 is found
strata_pack = "~/models/strata/packs/iq2_xs"
strata_mtp = "~/models/strata/mtp/rt"
# strata_profile = ""                  # default: the profile embedded in judas
ctx_total = 65536                       # --max-context
# strata_kv = "int8"                    # --kv
# strata_kv_resident = 32768            # --kv-resident
# strata_vram_reserve_mib = 3072        # --vram-reserve-mib (VRAM left for the desktop)
# strata_resident_budget_gib = 0        # >0: --mmap-experts --resident-budget-gib N (RAM for experts off the GPU)
# strata_spec = 4                       # --spec
# strata_spec_min_p = 0.5               # --spec-min-p
# strata_extra_args = []                # appended to the engine command line
[models.titan.sampling]
temp = 0.7
top_p = 0.95
top_k = 20
```

`strata_cwd` (optional) sets the engine's working directory. Defaults reproduce Strata's `strata-iq2_xs.json` (the 47 tok/s run): expert cache `auto`, prefill `auto`.
Anything else is `strata_extra_args`. `judas config check` reports missing `strata_*` files. The engine log is
`<data_dir>/logs/strata.log`;  Greedy decode when `temp = 0`.

## Build

```
cmake -S . -B build-strata -G Ninja -DCMAKE_BUILD_TYPE=Release -DJUDAS_STRATA=ON \
      -DLLAMA_DIR=~/src/llama.cpp -DLLAMA_LIB_DIR=~/src/llama.cpp/build/bin
cmake --build build-strata && (cd build-strata && ctest)
```

Options: `JUDAS_STRATA_DIR` (default `~/src/strata`), `JUDAS_STRATA_ROCM` (default `/opt/rocm`),
`JUDAS_STRATA_HIP_ARCH` (default: first `gfx` from rocminfo, else gfx1030). The first build compiles Strata's HIP
kernels (about a minute with all cores). `JUDAS_STRATA` defaults to OFF; the default build compiles none of it
(only `strata_proto` and the config keys, which are inert) and a config with `backend = "strata"` fails to start
with a clear message. `tests/strata_tokenizer_check.cpp` (ON only) runs the tokenizer ids and the whole backend
against a fake engine process (`tests/fixtures/fake_strata.py`, selected by the test-only env
`JUDAS_STRATA_FAKE_ENGINE`), on the CPU.

## Testing on the GPU

`tools/test-strata.sh` builds `build-strata` if needed, stops `judas.service`, loads the model through this backend on port 18081 with a temp
config and data dir, runs a short chat and a 400-token decode, prints tok/s, and restores the service.

## Not done / ideas

MTP acceptance and expert-cache hit rates (`DONE` fields) are parsed but not shown in the UI. A silence watchdog,
`PP`-based progress in `/api/state`, `logit_bias` through Strata, a multi-agent scheduler that groups requests by
conversation to keep checkpoints warm, and vision through `strata-vision` are the follow-ups.
