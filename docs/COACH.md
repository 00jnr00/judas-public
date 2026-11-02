# Coach (P0: propose-only)

Coach reads a past session and proposes small changes to the agent's own guidance, so the next session
starts a little smarter. It is propose-only: nothing is re-run, and nothing changes until you adopt a proposal.

Inspired by Harness^2 (Google Research, arXiv 2610.14224), which improves the harness around a frozen model
by proposing, probing and composing edits. This is a clean-room, much smaller take: only the propose step,
on a transcript that already exists. Probing (re-running a task under a candidate) is a separate, explicit
step; see Probes below.

## Turn it on

```toml
[coach]
enabled = false            # set true to propose, and to include adopted rules in the agent prompt
overlay = ""               # "" = <data_dir>/coach/overlay.md
overlay_max_chars = 1200   # cap on one proposal's prompt overlay
tools_max_chars = 800      # cap on one proposal's tool notes
max_skills = 3             # skills one proposal may carry
```

## Use it

| Where | How |
|---|---|
| CLI | `judas coach <session-id>` or `judas coach last` (needs the daemon), then `judas coach list`, `show ID`, `adopt ID`, `reject ID` |
| TUI | `/coach` (this session) or `/coach SESSION`, then `/coach list`, `/coach show ID`, `/coach adopt ID`, `/coach reject ID` |
| Web | the **Coach** tab: pick a session, read the proposal, Adopt or Reject |
| API | `POST /api/coach {session}` (202, runs in the background), `GET /api/coach`, `GET /api/coach/<id>`, `POST /api/coach/<id>/adopt`, `POST /api/coach/<id>/reject` |

`list`, `show`, `adopt` and `reject` read and write plain files and work without the daemon. Proposing needs
the loaded model: one hidden call at background priority (it yields to anything you are doing) with the
transcript trimmed to fit the context, shrinking tool outputs first, then dropping the middle of the session
and keeping the task and the latest steps. Tool results that mention a `permissions.secret_paths` entry are
withheld from the transcript.

## What a proposal contains

Each proposal is a directory `<data_dir>/coach/<timestamp>-<session>/` of plain files:

```
README.md     human-readable proposal: summary, every edit with its rationale, discarded edits
meta.json     the same, machine-readable (status: proposed | adopted | rejected)
overlay.md    rules to add to the agent prompt
tools.md      one-line notes on using named tools
skills/*.md   ordinary skills (frontmatter source: coach)
```

An edit is one of: a prompt overlay (a few short rules), a skill (Markdown how-to), or a tool note. Every
edit carries a one-line rationale that cites transcript steps such as `[12-15]`; an edit without such a
citation is discarded. Steps are numbered in the order the transcript shows them.

## Adopting

`adopt` copies each skill into the skills directory (never over a skill that already exists) and appends the
overlay and tool notes to the overlay file under a `## coach <id>` heading. While `[coach] enabled = true`,
the agent prompt includes that file (newest sections first, capped at 4000 characters). To undo an adoption,
delete the skill file or the section in the overlay file; both are plain text.

## What a proposal can never do

Variants cannot change permissions, the sandbox, approvals, privilege escalation, config files or the safety
floor. This is enforced in code, not in the prompt: edits whose name or text mention those are discarded when
the proposal is written, every file is checked again when you adopt (people may edit the files in between),
and an overlay section that matches is dropped when the prompt is built. The check is deliberately broad, so
a skill that merely talks about permissions is refused too. Skills also go through the skills guard.

## Reply format (what the model is asked for)

```
SUMMARY: one or two sentences
=== OVERLAY ===
RATIONALE: [12-15] why
- rule
=== SKILL short-name ===
DESCRIPTION: one line
TRIGGERS: word, another word
RATIONALE: [3] why
Markdown body
=== TOOL tool_name ===
RATIONALE: [7] why
the note
=== END ===
```

## Probes

A probe tests a proposal that is not adopted yet. It runs one task twice, once under the base harness and once
with the proposal, each in its own scratch copy of a fixture directory, and prints a table of mechanical signals.
It never touches your real working directory.

```
judas coach probe ID --task "TEXT or @file" --fixture DIR [--check "CMD"] [--k 1] [--timeout 1800] [--mode code]
POST /api/coach/probe {proposal, task, fixture, check?, k?, timeout?, mode?}    (202, background)
```

Probing needs the daemon and the loaded model, and `[coach] enabled = true`. Poll `GET /api/coach`; the job is
listed as `probe:<id>`.

For each of `k` rounds the base arm runs first, then the variant arm, one after the other (a single-sequence
backend cannot run two at once). Each arm gets a fresh copy of the fixture (`cp -a --reflink=auto`) under
`$XDG_CACHE_HOME/judas/probes/<id>/<round>-<arm>/work` (default `~/.cache/judas/probes`). The scratch trees never go
under `data_dir`, because tools refuse to write to Judas's own data and an arm working there could not edit anything. Each run gets a new session titled `[probe] <id> <arm> r<round>` (the
title prefix is the only marker; sessions are not hidden from the list). The task runs as an ordinary lead agent.

What differs between the arms, for that session only:

- the variant's system prompt gets the proposal's `overlay.md` and `tools.md` appended after the globally
  adopted overlay (which applies to both arms equally);
- the variant can see the proposal's skills as an extra skill source; a skill of the same name wins over an
  existing one. Skills that fail the safety floor or the skills guard are left out.

Both arms also run with the background learning review off and as an autonomous actor for pinned and user-owned
skills, so a probe does not write memory or change protected skills. Permissions, the sandbox and the safety
floor apply to probe sessions exactly as to any session; a proposal whose files touch the safety floor is refused
before anything runs. A probe is not a stronger sandbox than your permission settings: point `--fixture` at a
directory that is safe to edit, and use a task whose side effects stay inside the working directory.

After each run the harness records the lead's status, wall time, tool calls, writes, the longest run of
read-only calls, harness nudges and tokens (the same facts `coach_signals` shows the proposer). If `--check` is
given it runs in that arm's work dir with a 300 s timeout, outside the model, and its exit code and the last 40
lines of output are kept. A run passes when the check exits 0; with no check, when the lead finished (`done`).

Output, next to the proposal:

```
probes/evidence.md    table: arm, round, status, check exit, wall s, tool calls, writes, longest read streak, nudges, tokens
probes/evidence.json  the same plus signals and check output
~/.cache/judas/probes/<id>/<n>-<arm>/work  the scratch trees (kept for you to diff; delete when done)
```

`meta.json` gets `"probed": true` and a one-line verdict computed from the numbers, never by a model:
**variant better** when its pass rate is higher, or equal and above zero and it used at least 20% fewer tool calls in total;
**base better** the other way round; otherwise **no clear difference**. With `k = 1` the verdict is marked low
confidence: one sample per arm is mostly noise. `judas coach show ID` and the web Coach tab print the table. A
second probe of the same proposal replaces the first.

## Not in this version

Not yet: a composer that judges edits against evidence, a
library with per-domain retrieval, revert by id, idle or automatic triggering, and role or knob variants.
