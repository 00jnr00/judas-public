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
3. **Progress.** The plan, phase and noted propositions of each topic are kept by `learn_plan` and `learn_note`
   (see below) in `<data_dir>/learn/topics.json`. A new session on the same topic picks them up. The older
   `learn_progress` tool is retired; its file `<data_dir>/learn/progress.json` is read once and folded into the new
   store (see Migration).
4. **Long-term facts** go to the normal `[memory]` file through `memory_search` / `memory_append`; Learn does not
   keep a second store for them.

## Cards, plan and knowledge (Learn v2)

Learn sessions use four more tools, and the web app and the phone UI draw them with `web/learn/learn.js` and
`learn.css` (shared by both).

- `learn_ask {kind: predict|check|probe|practice, prompt, options?, want_confidence?, want_thinking?}` shows a
  question card and ends the turn. The card has the prompt (markdown), answer buttons or a free answer box, an
  optional 4 step confidence control (guessing, unsure, fairly sure, certain) and an optional "Think out loud" box.
  Submitting sends a readable message (`Answer: ...`, `Confidence: ...`, `Thinking: ...`); the answered card folds
  to one line.
- `learn_plan {topic, phase?, steps?, update?}` keeps the phase (measure, plan, teach, practice) and the plan of a
  topic: steps with `id`, `title`, `depends_on[]` and `status` (todo, now, done, repair). `steps` replaces the plan
  (unknown ids and dependency loops are refused); `update` changes statuses only. The UI shows a phase strip and the
  plan as a vertical stepper (phone) or a small node diagram (wide screens), and refreshes when the tool finishes.
- `learn_note {topic, notes: [{text, premises[]}]}` records 1 to 3 short propositions (at most 60 per topic). Every
  Learn session gets a capped digest of them in its prompt. The Knowledge view lists topics, propositions with
  their premises and a ring graph; Practice starts a new Learn session that quizzes from them.

Premises and links. A premise says what a proposition rests on. It can be plain words, or the short id of an
earlier proposition on the same topic (`p2`; `P2` and `#p2` also work). Words are matched to propositions after
normalizing: case, punctuation and runs of whitespace do not matter, so "Chaining stores colliding keys." matches
"chaining stores colliding keys". A premise that matches is saved as that proposition's id, shown with its text,
and drawn as a link in the graph; one that matches nothing is a leaf dot. An id that does not exist on the topic is
refused so the model can fix it, and a proposition never counts as its own premise. Saying the same claim again (same
words after normalizing) updates the existing proposition instead of adding a copy.

Migration. The first time `topics.json` is loaded and `progress.json` exists, each old topic becomes a new one
(unless a topic with that name already exists): its plan steps become a chain of steps (done ones stay done), and
its goal, held ideas, gaps and practice results become propositions. A flag in `topics.json` makes this happen once;
`progress.json` is never modified or deleted.

Storage: `<data_dir>/learn/topics.json`. API: `GET /api/learn/topics` (id, name, phase, steps_total, steps_done,
props, updated, session) and `GET /api/learn/topic/<id>` (full topic, 404 if unknown).

## Configuration

```toml
[learn]
context_files = ["~/notes/goals.md", "~/notes/projects/*.md"]   # paths or globs, ~ expanded; default empty
max_context_tokens = 3000                                       # cap on the digest in the lead's prompt
show_visual = false                                             # true brings back the visual tutor switch below
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

Hidden unless `[learn] show_visual = true` (default false; the cards, plan and knowledge view replace it). With it
off, `/api/learn/visual` reports an empty `url`, the frame permission is not added and the forwarder does not start.

The Learn screen can show a separate visual tutor app (for example LearnHarness, which is AGPL and runs as its own
process, so Judas only frames it) next to the tutor chat. The Learn strip then has a Tutor chat / Visual tutor switch.

```toml
[learn]
show_visual = true
visual_url = "http://localhost:8024"   # embedded in the Learn screen; its origin is added to frame-src
visual_label = "Visual tutor"          # switch label
visual_unit = "learnharness"           # optional systemd user unit, started on demand when you open the view
visual_proxy_port = -1                 # forwarder for other devices: -1 = visual_url port + 100, 0 = off, else a port
```

`GET /api/learn/visual` reports `{url, label, unit, up, proxy_port}`; `POST /api/learn/visual {"on": true|false}` starts or stops
the unit. Judas never enables the unit at login.

### Phones and other devices (forwarder)

`visual_url` usually says `localhost`, which on a phone is the phone. So Judas also listens on `proxy_port` (same bind
addresses as the main server, so a tailnet `extra_binds` address works) and forwards every request to the origin of
`visual_url`, and only that origin. The web app and the phone UI use it automatically whenever the page was not opened
on `localhost` (`http://<host the page came from>:<proxy_port>/`); on the server machine they still frame `visual_url`.

Why a port and not a path such as `/learnharness/`: the tutor is a Next.js app whose pages link to `/_next/...` and
`/api/...` with absolute paths and which cannot be rebuilt with a base path (it must run unmodified), so under a
sub-path every asset and API call would 404 unless the proxy rewrote HTML and JavaScript. A dedicated port keeps the
app at its own root and needs no rewriting.

The forwarder passes GET, HEAD, POST, PUT, PATCH and DELETE, streams bodies and responses in both directions (event
streams work), rebuilds multipart uploads, rewrites `Origin`/`Referer` to the upstream origin and a same-origin
`Location` to a relative one, drops the upstream's frame-blocking headers (Judas pages frame it, and the main pages'
CSP allows `http://*:<proxy_port>`), and applies the same Host check as the main server (`server.allowed_hosts`).
It is plain http: a page opened over https (for example through `tailscale serve`) cannot frame it, and the phone UI
says so and offers the address. Anyone who can reach that port can use the tutor, including its settings, so bind it
only where you would already expose Judas. With the tutor not running the phone UI says so and shows
`systemctl --user start <unit>` plus a Start button when `visual_unit` is set.
