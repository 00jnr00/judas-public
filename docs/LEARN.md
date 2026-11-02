# Learn mode

Learn is a fourth session mode (next to Chat, Cowork and Code) for understanding something rather than getting an
answer. The lead agent acts as a tutor: it reads what you have written about your goals, asks a few questions to
find where your understanding stops, plans the shortest route to your goal, and teaches in small checked steps.

## What it does

1. **Orient.** The lead gets a digest of your context files in its prompt and can search or read them on demand with
   the `learner_context` tool, so it does not ask you for things you already wrote down.
2. **Probe, plan, teach, practice.** The loop is driven by the system prompt (`learn_prompt()` in
   `src/tutor/tutor.cpp`): short questions about your reasoning, a plan of a few lines, one new idea per step ending
   in one question, repair before moving on, later practice problems that do not name the idea.
3. **Progress.** The `learn_progress` tool keeps a small record per topic (goal, what you hold, gaps, plan with steps
   done, practice results) in `<data_dir>/learn/progress.json`. A new session on the same topic picks it up.
4. **Long-term facts** go to the normal `[memory]` file through `memory_search` / `memory_append`; Learn does not
   keep a second store for them.

## Configuration

```toml
[learn]
context_files = ["~/notes/goals.md", "~/notes/projects/*.md"]   # paths or globs, ~ expanded; default empty
max_context_tokens = 3000                                       # cap on the digest in the lead's prompt
```

A glob matches one path segment at a time (`*`, `?`, `[a-z]`); a listed directory stands for its direct text files.
Each file gets an equal share of the digest budget and shows its head; the rest is reachable with `learner_context`.

## Read only

Context files are never written by Judas. Learn sessions get a read-only toolset (no `write_file`, `edit_file`,
`shell`, snapshot revert or MCP tools), and `write_file` / `edit_file` refuse a context file in every mode. Do not
list the memory file itself in `context_files`: it is already in the prompt and the memory tools are the way to
change it.

## Where it came from

The idea of a harness that measures understanding before teaching and teaches one reasoning step at a time was
inspired by [LearnHarness](https://github.com/amosblomqvist/learnharness). This implementation is independent: no
code, prompts or data formats were taken from it (it is AGPL-3.0; Judas is MIT).

## Visual tutor (optional)

The Learn screen can show a separate visual tutor app (for example LearnHarness, which is AGPL and runs as its own
process, so Judas only frames it) next to the tutor chat. The Learn strip then has a Tutor chat / Visual tutor switch.

```toml
[learn]
visual_url = "http://localhost:8024"   # embedded in the Learn screen; its origin is added to frame-src
visual_label = "Visual tutor"          # switch label
visual_unit = "learnharness"           # optional systemd user unit, started on demand when you open the view
```

`GET /api/learn/visual` reports `{url, label, unit, up}`; `POST /api/learn/visual {"on": true|false}` starts or stops
the unit. Judas never enables the unit at login.
