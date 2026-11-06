# Agent loop guard

Local models fail in a few repeatable ways. The turn loop (`src/agent/agent.cpp`) catches each one with a pure
decision function in `src/agent/policy.cpp`, so the behavior is unit tested with scripted model output
(`tests/test_loop_guard.cpp`, `tests/test_agent_policy.cpp`, `tests/test_autoresume.cpp`).

## 1. Read loops

`LoopGuard` remembers the last 16 tool calls (tool name plus arguments). The same call `loop_repeats` times (default 3),
or the same normalized error, is a loop. Two calls taking turns (`A B A B A`) count as one loop and are reported as
a `cycle`.

- First time: a short harness message is added after the tool results ("you already have that result: act on it,
  or finish"). The evidence is cleared, so a new warning needs fresh repeats.
- If it continues (`loop_pause_after` warnings close together, default 2): the turn pauses with a reason such as
  `looping: the same read_file call kept repeating after a warning` or `... calls kept alternating ...`.
- A successful write resets the evidence: re-reading after an edit is fine. `done`, `update_plan`, `wait_agents` and
  similar tools are never tracked.

The read streak (`read_streak_nudge`, `read_streak_force`) is separate and unchanged.

## 2. Tool call written as text

When a reply has no real tool call but its text holds one, the harness parses it and runs it as a real call. Formats:

- `<tool_call><function=shell><parameter=command>...</parameter></function></tool_call>` (several calls allowed, text
  before the first tag is kept as the message).
- JSON `{"name": ..., "arguments": {...}}` (`parameters` also accepted, arguments may be JSON text), alone, in a
  code fence, in `<tool_call>` tags, or as a list.

Only tools advertised to the agent are accepted. A JSON reply must be nothing but the call, so prose that quotes an
example is never run. Parameter values are strings; they are converted to numbers, booleans, arrays or objects from the
tool's schema. The call then goes through the normal path: advertised-tool check, hooks, approvals, the shell safety
floor and the journal. A call that is cut off, names an unknown tool or has a broken parameter does not run; the model
is asked to retry with a real tool call (counted against `max_auto_continues`).

## 3. Empty and no-progress turns

- Inside a turn: after 2 empty replies in a row the harness tells the model to wrap up (summarize what is done and what
  is left, no tool call). A plain reply then ends the turn even with plan items open. A further empty reply pauses it.
- Across auto-resumes: after 2 resumes in a row that changed no file and ticked no plan item, the next auto-resume is a
  wrap-up request (summarize and call done). After 3, auto-resume stops.

## 4. Per-turn wall-clock budget

```toml
[agents]
turn_wall_s = 0     # 0 = off
```

When a turn has run for `turn_wall_s` seconds, the next step is a tool-free wrap-up (the same instruction as above) and
the turn ends with that summary as `done`. The check runs between model steps; a single long generation or tool call is
not interrupted.

## Config

| Key | Default | Meaning |
|---|---|---|
| `agents.loop_repeats` | 3 | identical calls or errors in the window that count as a loop |
| `agents.loop_pause_after` | 2 | loop warnings before the turn pauses |
| `agents.max_auto_continues` | 8 | harness nudges per turn (text-call retries and empty replies count) |
| `agents.turn_wall_s` | 0 | wall-clock budget per turn in seconds (0 = off) |
