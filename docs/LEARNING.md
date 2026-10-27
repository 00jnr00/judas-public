# The learning loop

Judas can review a finished lead turn in the background and keep what it learned. The first shipped piece is
background review: a hidden agent reads the same conversation and may write memory or an agent-authored
skill. The curator and `/learn` workstreams build on the same `[learning]` table.

## Configuration

```toml
[learning]
review = true              # run a background review when a nudge counter reaches its threshold
review_tool_steps = 10     # tool-calling steps since the last review (skill nudge)
review_memory_turns = 10   # lead turns since the last review (memory nudge)
review_max_steps = 12      # model steps the review agent may take
```

A threshold of `0` disables that nudge. `review = false` disables the whole feature. The values are parsed,
dumped by `judas config show`, and present in the `judas init` template.

## When a review runs

The counters live per session in `AgentManager`. Tool-calling steps increment `tool_steps` as a lead turn
runs. A lead turn that finishes `done` increments `turns`. After that turn a review starts only when:

- `[learning].review` is true;
- the turn is the session lead's own turn, not a worker, reviewer, or the review agent;
- the turn ended `done`, not cancelled, paused, or failed;
- no review is already in flight for the session;
- no new user message is already queued;
- at least one nudge counter reached its threshold.

The memory prompt fires at the turn threshold; the skill prompt fires at the tool-step threshold. Both
thresholds on the same turn produce the combined prompt. A review consumes only the nudge(s) it used, so the
other dimension can fire on a later turn. A new user turn on the session cancels an in-flight review.

## How a review runs

The review is a hidden agent row with role `review` and the lead as its parent. Its message list is a copy of
the lead's system prompt and history plus one user message: the review prompt. Copying the prefix lets the
engine reuse the lead's cached KV instead of paying for a cold replay.

The review agent uses the lowest engine priority (1000000, larger than any role), so it never crowds out a
user turn. It sees only these tools, when registered:

- `skill_manage`
- `load_skill`
- `memory_read`, `memory_search`, `memory_append`
- `session_search`

No hooks fire for the review: no pre-tool, post-tool, pre-edit, post-edit, task-done, or turn-end. Its deltas,
tool calls and messages are not streamed or persisted. When it finishes, the manager emits one `review` event
with a short summary of what it wrote, or `nothing to save`.

## The autonomous guard

The review agent has no user present, so `ToolContext.autonomous` is set. With that flag, `skill_manage`
refuses every write to a skill whose `source` is not `agent` and to every pinned skill, with
`protected: user owned`. The review can still create a new skill (it gets `source: agent`) and patch an
unpinned agent-authored skill. A foreground agent is unaffected and can still edit user-owned skills.

## Prompts

`src/learning/review.cpp` holds the memory-only, skill-only, and combined prompts. They are trimmed ports of
the Hermes prompts for the tools Judas has: no support files, no curator adopt, and no `/skill-name` command.
The skill prompt keeps the "Do NOT capture" list (environment failures, negative tool claims, transient
errors, one-off narratives, unresolved failures) so a review does not harden a temporary problem into a rule.
