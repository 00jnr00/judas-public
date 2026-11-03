# Swarm teams

Swarm mode becomes a team runtime. The user talks only to the lead agent. The lead organizes the work into teams of
agents, runs them, merges their reports and answers. Every agent streams live into the Swarm screen. The CPU bulk
tool (`swarm_map`, docs/SWARM.md) stays, as one capability a team member can use.

Status: P1 (team model), P2 (orchestration), map files and P3 (Swarm screen) built and unit tested
(tests/test_teams.cpp, tests/test_team_runtime.cpp, tests/test_remote_engine.cpp). Proof runs are at the end.

## Sources and what we took

The team model takes ideas only. The Swarm screen ports UI code from both projects: card anatomy, activity ring,
hot-potato edges, layout rule, glyphs, view tabs, table shimmer and preview popover from OpenRig; the agent list,
focus border and pane status line from herdr. Both are Apache-2.0 (checked 2026-11-02; herdr was listed as AGPL in
older notes, its repository now says Apache-2.0). Ported blocks in web/app.js and web/app.css carry an attribution
comment naming the source file and saying they were modified; the license text is in LICENSES/Apache-2.0.txt and
THIRD_PARTY.md lists both. Judas stays MIT.

| Source | Taken (as an idea) | Skipped, and why |
|---|---|---|
| herdr (github.com/ogulcancelik/herdr, now herdrdev/herdr) | A grid of agent panes side by side with tabs; one place to supervise many agents; a per-agent state that is detected, not self-reported (working, waiting, idle, blocked) so the stuck one stands out; agents that spawn helpers and read each other's output through an API | Terminal multiplexing, detach and reattach, remote machines, plugins: Judas owns the agent loop itself, so there is no terminal to host. Typing into a worker pane: the user talks only to the lead. |
| OpenRig (github.com/mvschwarz/openrig) | The RigSpec shape: pods of members with typed edges between them (who delegates to whom, who may observe whom) and a shared culture file every member gets; the AgentSpec idea of a reusable blueprint (role, skills, guidance) instantiated per member; startup guidance delivered by the harness, not by the lead's prose | YAML, profiles, imports, plugin and runtime resources, services, checkpoints, continuity policies across process restarts, per-member runtimes and permission policies. Judas has one model process and one permission system; a team must never carry permissions. |

## Model

A team is: a name, a brief (the shared culture: goal, constraints, output format), house rules, members and edges.

```toml
# ~/.config/judas/teams/research.toml  (or built on the fly by the lead with team_create)
name = "research"
brief = "Answer the user's question from primary sources. Every claim cites a saved file."
rules = ["Quote numbers exactly as the source gives them."]
rule_files = ["SPEC.md#House rules"]     # a file, or one Markdown section of it, pinned verbatim

[[members]]
id = "editor"          # the team lead: gets the team's task, sub-delegates to the members below
role = "lead"          # team lead (a worker that may assign inside its own team)
[[members]]
id = "reader"
role = "worker"
count = 3              # reader.1 .. reader.3
skills = ["web-research"]
model = "gpu"          # gpu (main model) | cpu (the swarm instance's small model)

[[edges]]              # optional extras; the default edges always exist
from = "reader"
to = "reader"
kind = "observe"       # message | observe
```

Default edges, always present: session lead -> team lead (or -> every member when the team has no team lead), team
lead -> its members, every member -> its parent (reports up). Extra edges only add `message` or `observe` links
inside a team. A member may always read its own parent's brief; it can only read a teammate's output with an
`observe` edge or as that teammate's parent.

A team spec has no field for permissions, sandbox, approvals or tools beyond skills. Unknown keys are an error, and
the safety floor (sudo, su, doas, pkexec, passwd, faillock blocked in the shell tool) is enforced by the shell tool
for every agent regardless of team.

## Map files

A team map is a plain text file, one fact per line, written by you or by the lead. It is the format small models
write best: no nesting, no brackets, no indentation that matters, and a bad line is reported and skipped, never
fatal. The harness loads it as the team (or teams) and rewrites it in a clean, canonical form; the Swarm screen
draws exactly that structure.

```
# research.map  (~/.config/judas/teams/research.map, or passed inline to team_create as map)
team research
brief Answer the question from the sources. Cite every file.
brief Write the report to REPORT.md.          # more brief lines are joined
rules SPEC.md#House rules                     # a file or one Markdown section, pinned verbatim
rule Quote numbers exactly as the source gives them.
lead editor                                   # the team lead (optional, at most one)
member reader x3                              # copies reader.1 .. reader.3, gpu by default
member triage cpu skills=web-research         # cpu = the swarm instance's small model
member check role=reviewer
link reader -> reader observe                 # extra edges: message (default) or observe
place 1: editor                               # optional layout rows for the map
place 2: reader.1 reader.2 reader.3 triage

team ops                                      # a second team in the same file starts here
member runner x2
```

Rules of the format:
- The first word is the keyword, case does not matter: `team`, `brief`, `rule`, `rules`, `lead`, `member`, `link`,
  `place`. `#` starts a comment. Blank lines are ignored. A line before the first `team` line belongs to a team
  named after the file.
- `member ID [xN] [gpu|cpu] [role=ROLE] [skills=a,b]` in any order after the id; `lead ID [skills=a,b]`.
- `link FROM -> TO [message|observe]`; `->` may also be written `to`.
- `place N: COPY COPY ...` pins row N of the map. Copies not placed go in an extra last row.
- Anything else is a warning: the line is skipped and listed back to the writer. Words about permissions, sandbox,
  tools, sudo or approvals are always skipped; a team cannot change what an agent may do.

`team_create {map: "..."}` takes the text inline, `team_create {spec: "research"}` loads
`~/.config/judas/teams/research.map` (or `.toml`). The tool result carries the canonical text and the warnings.

## Why new tools, not a bigger spawn_agent

`spawn_agent` is fire-and-forget: a task string and a role. A team needs state that outlives one call (member ids,
edges, the pinned brief, per-member status) and checks on every message (edges). Putting that behind optional
spawn_agent arguments would make a simple tool harder for small models, and the edge check would have no object to
hang on. So teams get their own tools and spawn_agent stays as it is for ad hoc helpers.

| Tool | Who | What |
|---|---|---|
| `team_create` | session lead | `{map: "..."}` (one or more teams as map lines), `{spec: "research"}` to load a saved `.map` or `.toml`, or the JSON fields `{name, brief, rules?, rule_files?, members, edges?, place?}`. Returns the canonical map and skipped lines. Agents are created on first assignment. |
| `team_assign` | session lead, team lead | `{team, member, task}`. `member` is an id (`reader` picks the first idle copy) or a copy (`reader.2`). Starts or continues that agent. |
| `team_status` | anyone in the session | members with status, tokens and the head of their latest output. |
| `team_message` | along an edge | `{to, message}`. Queued for the target's next turn (starts it if idle). |
| `team_read` | along an observe edge or to a child | `{agent, max_chars}` the tail of an agent's latest output, bounded (default 4000, cap 12000). |
| `team_wait` | session lead, team lead | `{team, timeout_s}` waits for every running member you assigned, returns their reports. Auto-wait still applies. |
| `team_disband` | session lead | cancels the team's agents and closes the team. |

## Pinning (the fix for lost house rules)

Every member gets one harness-owned system block right after its system prompt, rebuilt before every model step,
re-inserted if compaction ever drops it (the same mechanism as the plan block):

```
[team research] you are reader.2 (worker), reporting to editor.
Brief: ...
House rules (verbatim, apply to every piece of work):
- ... (from rules, rule_files and [teams] house_rules in config)
User request (verbatim): ... (the lead's current turn input, capped)
Edges: you may message editor; you may observe reader.1, reader.3.
```

The lead does not restate the rules in each hand-off; the harness copies them from the source. Our evals showed
this is where worker mode lost points (codebench hard --workers 0.72, rule A at 0.75).

## Status

Each agent has a live activity, combined with the engine's sequence phase for that agent's tag:

| Status | Meaning |
|---|---|
| queued | waiting for a sequence slot or KV admission (engine queue, or Strata's single sequence) |
| prefilling | the engine is reading its prompt |
| generating | tokens are streaming |
| tool | a tool call is running |
| waiting | waiting on child agents (team_wait, wait_agents, auto-wait) |
| blocked | waiting for a user approval, or paused at a cap with work left |
| done / failed | the turn ended |

Agents emit an `activity` event on every change; `GET /api/sessions/<id>/team` returns the merged view
(teams, members, edges, status, tokens, tok/s) for a page that just loaded.

## Limits

- `agents.max_depth` still caps nesting: session lead 0, team lead 1, member 2. A team without a team lead puts its
  members at depth 1. A member at the cap gets no team or spawn tools.
- `[teams] max_members` (default 16) caps members per session across all teams; `engine.max_agents` and KV admission
  decide how many run at once, the rest queue in the engine. On the Strata backend there is one sequence, so
  members take turns; nothing in the team layer assumes parallelism.
- `model = "cpu"` members run on the swarm instance (`[swarm].url`) through an OpenAI-compatible remote engine,
  with a read-only tool set plus `team_message` and `done`, because small models are weak at editing tools. With
  the swarm off they fall back to the main model and the brief says so.

## UI

The Swarm screen keeps one composer (to the lead) and adds four views, picked by tabs (remembered per browser;
`?tab=map|panes|table|file` overrides):

- Map: a rig frame with the session lead on top and one dashed pod per team. Rows follow the map's `place` lines,
  else team lead first and members below. Cards show role, gpu or cpu, context %, tokens, glyph and state, with an
  activity ring (green working, amber waiting, blocked or paused, red failed). Edges are drawn from the team
  structure; every `team` event with `from` and `to` (assigned, message, report) sends a packet along its edge.
  Hover shows the agent's last output; click opens its pane. Wide screens shrink the map to fit (not below 0.8);
  phones scroll it inside its box, opened on the lead.
- Panes: an agent list (failed and blocked first) beside live panes with a focus border and a status line
  (folder, model, context bar, tok/s, tokens); Focus, Pause, Resume, Cancel, Transcript. One column on phones.
- Table: one sortable row per agent.
- File: each team's canonical map text and its skipped lines, with Copy.

Skin variables only, no build step. `tools/swarm-ui-shot.py` renders every view from a fixture at desktop and phone
width, including a mid-animation shot.

## Proof (P4)

Filled in after the runs.
