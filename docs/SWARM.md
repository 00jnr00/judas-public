# Swarm mode

Swarm mode is also the team runtime: the lead organizes big jobs into teams of agents on the main model (and on the CPU swarm for wide, shallow reading), and the Swarm screen shows every agent live. See [SWARM-TEAMS.md](SWARM-TEAMS.md). This page covers the CPU bulk tool, `swarm_map`, which any team member can use.

Bulk jobs (summarize, extract or classify many files or items) run on a swarm of small models on the CPU. The main model, which stays the only one on the GPU, only plans, checks and merges.

## How it works

- A second Judas instance (`judas-swarm.service`, port 8083, `contrib/swarm.toml`) serves a small Liquid LFM2.5 2.6B model on the CPU with 8 slots. It is the same pattern as the CPU instance and uses no VRAM.
- The main instance gets a `swarm_map` tool when `[swarm].enabled = true`. Arguments: `instruction` (becomes the system prompt), `items` (file paths relative to the working folder, globs such as `docs/**/*.md`, or literal text prefixed `text:`), and optional `output` (JSONL path; default `.judas/swarm/<timestamp>.jsonl` under the working folder).
- Files are read with the normal read permission checks, binaries and empty files are skipped, and files longer than `max_item_chars` are split into chunks (ids like `a.md#2/3`). Items are sent in parallel (`max_parallel`). The tool returns a compact list of per-item replies (cut short); the full replies are in the JSONL file. Progress shows as `swarm` events (done/total) in the web app.
- A session in the Swarm mode gets extra lead instructions: split bulk work into many small independent items, use `swarm_map`, keep its own work to planning, checking and merging.

## When to use it

The swarm is for wide, shallow work: many independent pieces that each need a quick read and a short answer. The main model does the thinking; the swarm does the reading.

| Job | How |
|---|---|
| Research | The lead searches and saves pages (web_fetch to files under the working folder), then one `swarm_map` pulls the facts, quotes and numbers that bear on the question from every page. The lead compares, resolves conflicts and writes the answer. |
| Big documents | Pass the file (PDF text, logs, transcripts, a book). It is split into chunks of `max_item_chars`; each chunk is summarized or searched in parallel, and the lead merges the chunk notes. A document far larger than the model's context gets read in one pass. |
| Many files | A folder of notes, emails, papers or source files: summarize each, tag or classify each, extract fields into one JSON object per file. |
| Triage | Sort a pile by relevance before the lead reads the few that matter ("score 0 to 3 for relevance to X, one line why"). |
| Data cleanup | Normalize names, dates or units across many records given as `text:` items. |

Not for: code changes, debugging, planning, math, or anything where one item depends on another. Those stay with the main model.

Patterns that work:
- Ask for a fixed format per item (one JSON object, or a short fixed list) so the lead can merge without rereading.
- Try 3 to 5 items first, check the replies, adjust the instruction, then run the rest.
- Map then reduce: swarm for the per-item pass, the lead for the final synthesis. For very large jobs, a second `swarm_map` over the first pass's notes (grouped) before the lead's final pass.
- The swarm can be wrong on a single item; the lead spot-checks a few replies against the source before trusting the merge.

## Delivery rules for the lead

A Swarm lead has to hand something over. These rules are enforced by the harness (`src/agent/policy.h`), not left to the prompt:

- **Report first.** When all workers are done (the harness waited for them, or `wait_agents` returned with none left running), the lead gets their results with the instruction to write the deliverable now: `write_file` to the requested path, or the final answer when no path was asked for. Until it has written the report (a `write_file` of at least 400 characters; edits and small scratch files do not count), `web_fetch`, `web_search` and shell commands that run `curl`, `wget` or `sleep` are refused with a message saying so. Reading, writing and every other tool stay available. After the report exists, verification is open again and the lead may refine the report.
- **Per-URL retry cap.** `web_fetch` calls, and shell `curl` or `wget` calls on the same URL, share one failure count per agent (the URL is compared without its fragment, trailing slash, or scheme and host case). Only calls that actually ran count; a call refused by the report-first gate does not. A call counts as a failure when the tool errors, when the result is a rate limit stub (`local_rate_limited`, 429, too many requests) or an empty JavaScript shell (almost no text, or an enable-JavaScript notice). After `url_retry_cap` failures (default 2) the next attempt on that URL is refused with a "Give up on this URL" result that tells the agent to use what the workers found or another source, and to mark the item unverified. A success clears the count. A shell command is judged by exit status and stub text only, so `curl -o file` with no output is not a failure.
- **Empty turns.** When the lead replies with nothing (no text, no tool call) `empty_turn_limit` times in a row (default 2), the harness injects "write the report now from what you have" and applies the same fetch and shell refusals. If the turn still pauses on empty replies, auto-resume sends that same message instead of the generic resume text.

Settings, in `[agents]`: `url_retry_cap = 2` (0 turns the cap off) and `empty_turn_limit = 2` (0 turns the empty-turn rule off). The report-first and empty-turn rules apply to the lead of a Swarm-mode session only; the URL cap applies to every agent.

## Enable it

1. `install.sh` copies `contrib/swarm.toml` to `~/.config/judas/swarm.toml` (only if absent) and the unit to `~/.config/systemd/user/judas-swarm.service` (only if absent). Neither is enabled at login. Check the model path in `swarm.toml`.
2. Turn the swarm on with the switch: in the Swarm tab (next to the status dot) or at the top of the Swarm section in Settings, or from a terminal with `judas swarm on`. `judas swarm off` turns it off and `judas swarm status` shows the state.

The switch does four things, live and without restarting the main `judas`: sets `[swarm] enabled` in `config.toml` (the rest of the file is kept), registers the `swarm_map` tool, runs `systemctl --user start <unit>`, and waits until the instance answers (up to 60 s, then shows an error). Off does the reverse: stops the unit, sets `enabled = false` and removes the tool (a session that still holds it gets "swarm is off"). It only starts and stops the unit; it never runs `enable` or `disable`, so start-at-login is unchanged. The unit name is `[swarm].unit` (default `judas-swarm`, letters, digits and `@ . _ -` only).

API: `GET /api/swarm/power` returns `{"enabled", "up", "unit"}`; `POST /api/swarm/power` with `{"on": true}` or `{"on": false}` switches it.

You can still set it by hand:

```toml
[swarm]
enabled = true
# url = "http://127.0.0.1:8083"   # max_parallel = 8   # max_item_chars = 24000   # unit = "judas-swarm"
```

then `systemctl --user start judas-swarm` and restart the main `judas`.

Open the Swarm tab (folder required). The strip above the chat shows whether the swarm is up and the job progress.

Small models are good at extraction, short summaries, classification and tagging; they are not for coding or long reasoning. The swarm instance has no auth, like the main one; keep it on localhost.

## Batching

The swarm instance is the same engine as the GPU one: `max_agents` sequences (up to 8) share one llama context with a unified KV pool, every generating request contributes its next token to a single `llama_decode` batch per step, and identical prompt prefixes are shared through prefix snapshots. Parallel requests are not serialized. On the CPU this pays because decode is memory-bound: one pass over the weights serves all sequences. Prefill is one chunk per step, so a long prompt delays the others a little.

## Cores

The swarm gets a core budget so it does not fight the GPU engine's CPU threads or your desktop.

- Web: Settings, "Swarm cores" (the slider is shown while the swarm is on). A slider from 1 to the number of logical CPUs and a "Pin to those cores" box.
- CLI: `judas swarm cores` shows the setting, `judas swarm cores 8 [--pin|--no-pin]` changes it.
- API: `GET/POST /api/swarm/cores` on the main server (`{"threads": N, "pin": true}`), which proxies to the swarm instance's own `GET/POST /api/cores`.

Where it lives: the swarm instance's own config (`[swarm].config`, default `~/.config/judas/swarm.toml`): `threads` of its active model (used for both the decode and the batch thread count) and `[engine] pin_cores`. The main server writes the file, then tells the running instance. A running instance applies it live: `llama_set_n_threads` between two decode steps (no reload, no restart, requests in flight continue) and `sched_setaffinity` on every thread of the process. If the instance is down, the value is saved and used at its next start (with `pin_cores = true` it pins itself before loading the model).

Pinning takes whole physical cores first, one CCD at a time, then the SMT siblings: on a 5900X that is CPUs 0-5, then 6-11, then 12-17, 18-23, read from `/sys/devices/system/cpu/cpu*/topology` and the L3 sharing list. So 6 cores is exactly CCD 0 with no SMT sharing.

## Benchmark

`tools/bench-swarm.sh` starts a temporary swarm instance from `build/judas` on port 18083 (temp config and data under `build/bench-swarm`, `gpu_layers = 0`, nothing of yours is touched), then sweeps cores (default 4 6 8 12 16 24, changed live) by parallel requests (1 2 4 8) with a fixed prompt and `max_tokens` 256, and adds a serialized baseline of 8 requests one after another. Thinking tokens count. It writes the table to `docs/SWARM-BENCH.md` and refuses to start when the load average is 4 or higher (`--force` overrides; the load is recorded in the output). Run it on an idle machine; it uses every core. Options: `--cores`, `--parallel`, `--max-tokens`, `--baseline-cores`, `--out`, `--model`, `--no-pin`. The load is generated by `tools/bench-swarm.py`.
