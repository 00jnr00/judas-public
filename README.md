# Judas

[Website](https://judas.sanchezlabs.us) · [Docs](docs/) · [Support on Ko-fi](https://ko-fi.com/sanchezperformance)

**The betrayer harness.** One C++ binary that loads one local model and runs many agents on it at once: model server, agent harness, terminal UI and web app, with every file change snapshotted so you can let the model work with real permissions and still undo anything.

- **One model, many agents.** Continuous batching on llama.cpp. A lead agent splits a job across workers and they all decode on the same weights, sharing their common prompt prefix in the KV cache. No model swapping, no routing, no second copy in VRAM.
- **Undo for everything.** Every write goes through a snapshot journal (reflink copies where the filesystem supports them). Diff or revert one step, or roll a whole session back.
- **Sandboxed tools.** Shell commands run in bubblewrap, writes are limited to allowed paths, and privilege escalation (`sudo`, `su`, `doas`, `pkexec`, `passwd`, ...) is blocked in code in every permission mode, including full-auto.
- **It learns.** Skills are plain Markdown files that load on trigger words. After long turns a background review saves lessons as skills or memory notes, and an idle curator archives skills that stopped earning their place.
- **OpenAI-compatible.** `/v1/chat/completions` and `/v1/models` for any client, plus a richer Judas API with server-sent events.
- **Three ways in:** a web app (Chat, Cowork, Code, Agents board, Tasks, Memory, Snapshots, Resources, Heartbeat), a terminal UI, and `judas run "task"` for scripts.

Measured on one RX 6800 (16 GB, Vulkan) with a 35B-A3B MoE: about 110 tok/s for one stream and about 270 tok/s total across 8 concurrent agents.

## Requirements

- Linux (x86_64). Other platforms are untested.
- A C++23 compiler (GCC 13+ or Clang 17+), CMake 3.20+, Ninja, and the SQLite 3, OpenSSL and toml++ development packages.
- A built [llama.cpp](https://github.com/ggml-org/llama.cpp) checkout (Vulkan, ROCm/HIP or CUDA backend). Judas links its `libllama` and `libllama-common`.
- `bubblewrap` (`bwrap`) for the shell and MCP sandbox. With `sandbox = "bwrap"` (the default) and no bwrap installed, shell commands and MCP servers are refused rather than run unconfined.
- A GGUF model that fits your GPU. Tool calling quality matters more than size; MoE models with a few billion active parameters work well.
- Optional: a SearXNG instance for the `web_search` tool, `git` for per-edit commits.

## Build and install

```sh
# 1. llama.cpp (pick one backend)
git clone https://github.com/ggml-org/llama.cpp ~/src/llama.cpp
cmake -S ~/src/llama.cpp -B ~/src/llama.cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON
#   ROCm: -DGGML_HIP=ON -DGPU_TARGETS=gfx1030     CUDA: -DGGML_CUDA=ON
cmake --build ~/src/llama.cpp/build --target llama llama-common

# 2. Judas
git clone <this repo> judas && cd judas
./install.sh            # builds, installs ~/.local/bin/judas, writes a user service and a starter config
```

Point Judas at a different llama.cpp with `-DLLAMA_DIR=/path/to/llama.cpp -DLLAMA_LIB_DIR=/path/to/build/bin`.

`install.sh` does not enable or start the service. Edit the config first.

## Get a model

Judas runs GGUF models (the llama.cpp format). If you already have some, Judas will find them:

```sh
judas models
```

```
GPU memory: 16.0 GB

   #  CONFIG         MODEL                                SIZE  QUANT      CTX  FIT      FROM
   1  -              Qwen3-30B-A3B-Q4_K_M.gguf         17.3 GB  Q4_K_M    256k  too big  lm-studio
   2  -              gemma-4-E4B-it-Q4_K_M.gguf         5.0 GB  Q4_K_M    128k  fits     huggingface
   3  -              ollama:qwen3:14b                   9.3 GB  Q4_K_M     40k  fits     ollama
```

It looks in `~/models`, LM Studio (`~/.lmstudio/models`), the Hugging Face cache, Ollama's store (`~/.ollama/models`, read in place, nothing copied), `~/.cache/llama.cpp`, GPT4All and `~/Downloads`. Add your own folders with `judas models --dir /path` or `JUDAS_MODEL_DIRS=/a:/b`. **FIT** compares the file with your GPU memory: `fits` loads fully on the GPU with room for context, `tight` loads with a small context, `too big` needs CPU offload (MoE models get `cpu_moe = true` automatically; dense models get slow).

Connect one:

```sh
judas models add 2 --use        # writes [models.gemma-4-e4b-it] and makes it the model the daemon loads
systemctl --user restart judas
```

`add` picks `ctx_total` and `ctx_per_agent` for the file and your GPU; edit them in the config afterwards if you like. `judas models info 2` shows the metadata and the block it would write.

No models yet? Find and download one from Hugging Face:

```sh
judas models search qwen3 coder                         # GGUF repos, most downloaded first
judas models files unsloth/Qwen3-30B-A3B-GGUF           # every quant with its size and fit
judas models pull unsloth/Qwen3-30B-A3B-GGUF --add --use    # no file named: the biggest quant that fits
judas models pull unsloth/Qwen3-30B-A3B-GGUF Q4_K_M --add --use
```

Downloads go to `~/models` (`--dir` to change), resume if interrupted, and handle multi-part files. Gated models (Llama, Gemma and others) need you to accept the license on huggingface.co and set `HF_TOKEN`.

What to pick: tool calling quality matters more than size. Mixture-of-experts models with a few billion active parameters (names like `30B-A3B`) are fast enough to run several agents at once. Q4_K_M is the usual quality and size balance; go lower only to make a model fit.

**Already running a model in LM Studio, Ollama or llama-server?** Judas can also use an OpenAI-compatible server instead of loading the model itself. You lose Judas's own batching and prefix sharing; how many agents run at once is then up to that server.

```toml
[models.remote]
url = "http://127.0.0.1:1234"        # LM Studio; Ollama is http://127.0.0.1:11434, llama-server http://127.0.0.1:8080
remote_model = "qwen3-14b"            # the model name that server expects
```

## Configure

`judas config init` writes `~/.config/judas/config.toml` with every option commented (`judas models add` creates it for you if it is missing). A model block looks like this:

```toml
[engine]
model = "main"
max_agents = 4

[models.main]
path = "~/models/your-model.gguf"
gpu_layers = 99
ctx_total = 65536        # one KV pool shared by all agents
ctx_per_agent = 16384
flash_attn = true
```

Check it with `judas config validate`, and see the effective settings with `judas config show`. `JUDAS_CONFIG`, `JUDAS_PORT` and `JUDAS_DATA_DIR` override the file, the port and the data directory.

Permission modes (`[permissions] mode`): `suggest` and `auto-edit` (default) ask you before actions that need approval, `full-auto` approves them itself. In every mode, writes stay inside `allow_paths`, shell commands run in the sandbox, every change is snapshotted, and privilege escalation is blocked.

## Run

```sh
systemctl --user start judas     # or: judas serve
judas                            # terminal UI
xdg-open http://127.0.0.1:8079   # web app
judas run "add a --verbose flag to cli.py and update the tests" --cwd ~/code/project
```

Judas refuses to load a model when the GPU already has more than 5 GB in use, to avoid resetting the GPU under another model or a game. `JUDAS_FORCE=1` overrides.

Other commands: `judas models`, `judas state`, `judas sessions`, `judas snap list|diff|revert`, `judas curator status|run`, `judas skills install`, `judas prompt-size`. Run `judas --help` for the full list.

## Security

The API has **no authentication**. It binds to `127.0.0.1` by default. Anyone who can reach the port can run commands as your user through the agents. Only add `extra_binds` on a network you fully trust (for example a private VPN), never on a LAN or the internet.

To keep web pages you visit from driving the API, Judas sends no wildcard CORS header, refuses requests whose `Host` is not this machine (`localhost`, the bind addresses, the hostname, or `server.allowed_hosts`), and refuses cross-origin writes unless the origin is listed in `server.cors_origins`. If you reach Judas by another name (a VPN hostname, for example), add it to `allowed_hosts`.

## Docs

- [docs/API.md](docs/API.md): Judas API, events, snapshots, tasks, OpenAI-compatible endpoints
- [docs/SKILLS.md](docs/SKILLS.md): writing skills and how they load
- [docs/LEARNING.md](docs/LEARNING.md): background review, curator, `/learn`
- [docs/MCP.md](docs/MCP.md): connecting MCP servers (sandboxed, fail closed)

## Testing

```sh
cmake -S . -B build -G Ninja && cmake --build build
JUDAS_TESTS_NO_NET=1 ctest --test-dir build
tools/test-rocm.sh --model ~/models/your-model.gguf   # AMD: HIP build + load + streaming + 4-stream decode (see GPU backend support)
```

## Status

Early release. Expect rough edges, and please file issues with your GPU, backend and model.

### GPU backend support

> **Vulkan** is the only backend Judas has been run on day to day, and only on one card (AMD RX 6800, 16 GB, RDNA2). Every number in this README comes from that setup.

> **ROCm / HIP: build-tested only.** `tools/test-rocm.sh` builds llama.cpp with HIP and runs the unit tests on gfx1030, and that passes. Loading a model and serving under ROCm has not been tested yet; the script's `--model` smoke test is there for that. ROCm support depends on your card, your ROCm version and llama.cpp's HIP backend, not on Judas. Other AMD targets (RDNA3, CDNA, APUs) are untested.

> **CUDA: untested.** Judas only calls libllama, so a llama.cpp built with `-DGGML_CUDA=ON` should work, but nobody has built or run Judas against it. Treat it as unsupported until someone reports back.

> **Metal / macOS, Windows, CPU-only:** untested. The sandbox needs Linux (bubblewrap), so non-Linux platforms would also need `permissions.sandbox = "none"`.

Throughput, VRAM fit and the default context sizes all depend on the backend and model. The VRAM guard (refusing to load when more than 5 GB is in use) reads AMD sysfs only, so on other GPUs it does nothing.

## Support

Judas is built and maintained by one person. If it saves you time or cloud bills, you can [buy me a coffee on Ko-fi](https://ko-fi.com/sanchezperformance).

## License

Not yet chosen. Third-party notices: [THIRD_PARTY.md](THIRD_PARTY.md).
