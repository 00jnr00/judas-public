# Terminal UI

`judas` (no arguments, on a terminal) opens the terminal UI: an inline-scrollback client of the daemon (`judas serve`) over HTTP and SSE, the same API the web app uses. Finished lines go into the normal terminal scrollback; a small live area at the bottom holds running tools, the input box and the status line. Type `/` for the command menu. Source is in `src/tui/`.

## Commands added by the sections below

| Command | What it does | Section |
|---|---|---|
| `/approvals` | List the pending approval requests for the session. | Approvals |
| `/team [watch]` | Swarm team map; `watch` keeps a live block until Esc. | Team |
| `/pause N\|copy` | Pause a team agent. | Team |
| `/resume` | Resume a paused turn. | Team |
| `/resume N\|copy` | Resume a paused or killed team agent. | Team |
| `/cancel-agent N\|copy` | Cancel one team agent after a y/n prompt. | Team |
| `/read N\|copy [chars]` | Print a team agent's latest output. | Team |
| `/snapshots [N]`, `/snapshots show ID` | Recent file-edit snapshots, or one diff. | Snapshots |
| `/revert ID [--to]` | Undo a snapshot (asks y/n). | Snapshots |
| `/theme [name]` | Pick a color theme live. | Theme |

## Key priority

Several features ask for a single key (y/n prompts, pagers). Only one owns the keyboard at a time, in this order:

1. A pending approval (including its pager). It always wins; typing is paused while it waits. Ctrl+C still cancels the turn.
2. The `/cancel-agent` y/n prompt.
3. The `/revert` y/n prompt.
4. The snapshot diff pager.
5. The slash-command menu and normal typing.

A lower prompt that was open when an approval arrives stays open and takes keys again once the approval is answered. The live area shows the prompt that currently owns the keys.

## Approvals
The daemon's permission mode (`[permissions] mode`) decides what asks for your yes: `suggest` asks for file writes and shell, `auto-edit` for shell, `full-auto` for nothing ordinary. The safety floor (sudo, su, doas, pkexec, passwd, faillock, auth databases) is refused outright and never reaches a prompt.

When an agent needs a yes for the current session, the TUI prints the request inline:

```
  approval needed  edit_file  (lead a-1)
  edit_file /home/me/proj/main.cpp
    -old line
    +new line
  Approve? [y]es / [n]o / [v]iew full
```

- Added lines (`+`) use the skin's ok color, removed lines (`-`) its error color. Long requests show the first 12 lines and a "more lines" hint.
- One key answers: `y` approves, `n` or `Esc` declines. `v` pages the full text into the scrollback (space or `v` for the next page, `q` to stop paging; `y` and `n` still work while paging).
- While a request waits, normal typing is paused: other keys are ignored, `Enter` included, so a stray return never approves. `Ctrl+C` still cancels the turn, which declines everything pending.
- Several pending requests (parallel agents, or another client) queue; the next prints when the one in front is answered. The prompt shows how many are waiting.
- One answer resolves a request everywhere: if you or another client answers it first, the TUI drops it and prints a one-line note.
- A request nobody answers is declined after 10 minutes, and a cancelled turn declines its requests.

`/approvals` lists the pending requests for the session (id, tool, agent, first line). The protocol (list, answer, events) is in docs/API.md under Approvals.

## Team

Pure helpers (parsing `GET /api/sessions/<id>/team`, formatting, hand-off notes, row lookup) with tests in
`tests/test_tui_team.cpp`. `tui.cpp` only holds the commands and the live block.

| Command | What it does |
|---|---|
| `/team` | Prints the lead line, then one block per team: name and the first line of its brief, team lead first, members indented. |
| `/team watch` | A live block above the input box, redrawn in place on `activity` and `team` events (and every 2 s for tok/s) until Esc or the turn ends; the last map is then printed to scrollback. Hand-offs print as one-line notes. Without a running turn it prints the map once. |
| `/pause N\|copy` | Pause that agent (resumable). |
| `/resume N\|copy` | Resume a paused or killed agent. Plain `/resume` still resumes a paused turn. |
| `/cancel-agent N\|copy` | Cancel that agent after a `[y] yes [n] no` prompt. Plain `/cancel` still cancels the turn. |
| `/read N\|copy [chars]` | Print the agent's latest output (its final result once done), 4000 chars by default. |

`N` is the row number printed by `/team`; the order is recomputed on every call (team lead first, then failed and
blocked, working, waiting, idle, done), so check `/team` before acting after a lot of activity. A copy name
(`reader.2`), an agent id or prefix, a unique member name or `lead` also work.

Row layout: `N glyph copy role model state ctx% tokens tok/s tail`. Glyphs and state words match the web Map tab
(`●` working or done, `◐` waiting, blocked or paused, `○` queued or idle, `✕` failed). The glyph and word are colored
by state (ok, warn, error, dim from the skin); ctx % by band (under 60 ok, 60 to 80 warn, over 80 error). The tail is
the current tool (`tool: web_fetch`) or the first line of the last output, cut to the terminal width, so wider
terminals show more of it. Copies in a team spec that have no agent yet show as `queued`.

Hand-off notes (`editor -> reader.2 assigned`, `reader.1 -> editor report`, `lead -> editor message`, plus
`team X created` and `team X disbanded`) are printed automatically from `team` events when the session mode is Swarm,
and during `/team watch` in any mode.

## Snapshots and revert

Every file edit the agent makes is journaled as a snapshot (see `docs/API.md`, Snapshots). The TUI reads and
reverts them through the same endpoints the web Snapshots screen uses.

| Command | What it does |
| --- | --- |
| `/snapshots [N]` | The last N (default 10, max 40) snapshots of the current session, newest first: time, agent, file count, `+added -removed` lines, label. Before the first message of a session there is no session, so it lists snapshots that touched files under the working folder. A reverted snapshot is marked, `!` marks irreversible side effects. |
| `/snapshots show ID` | Prints the snapshot's unified diff, added lines in the theme's `added` color, removed lines in `removed`, hunk and file headers in accent. Long diffs are paged: Enter or space for the next page, any other key stops. |
| `/revert ID` | Shows what will change (files, line counts, irreversible side effects), then asks. Only a typed `y` reverts; Enter, `n`, Esc or any other key cancels. Then it calls `POST /api/snapshots/<id>/revert` and prints restored, removed, conflicting and not-undone paths. |
| `/revert ID --to` | Same, but undoes this snapshot and every later one (the web app's "revert to here"). |

`ID` may be the full id, an unambiguous start (`20261102-14`) or an unambiguous end (`0007`).

The `+/-` counts and `/snapshots show` come from `GET /api/snapshots/<id>/diff`, which compares the snapshot's
before-state with the files as they are now. If a later snapshot edited the same file, its lines are included,
and a reverted snapshot shows no differences. This is the same number the web app shows.

## Theme

`/theme` opens a picker of every skin (the 16 embedded themes plus your files in `~/.config/judas/skins`), each
marked dark or light. Enter applies it at once: the prompt, status line and all new output use the new colors
(text already in the scrollback keeps the colors it was printed with). `/theme NAME` applies directly (name, label,
or a unique prefix).

The choice is saved to `~/.config/judas/tui.toml` (`theme = "name"`; other lines are kept; `config.toml` is never
touched) and used on the next start. Without that file the TUI uses `JUDAS_SKIN`, then `judas`.

`/theme sync` is not available: the web app keeps its theme choice in the browser's localStorage, and the server
only exposes the `JUDAS_SKIN` default (`GET /api/skins/current`), so there is nothing to follow.

### Light themes in a terminal

The TUI never paints a background color; it only sets foreground colors from the skin (text, dim, accent, ok,
warn, error, ...). A light skin therefore cannot put a white slab on a dark terminal. The terminal's
real background is unknown, so each foreground color is also checked for contrast (WCAG ratio: 4.5 for text, 3 for
dim) against the assumed background and lightened or darkened if needed. The assumed background is the terminal's
own when `COLORFGBG` is set (for example by rxvt-style terminals), else the skin's `bg`. When a light skin is
applied and the terminal is dark or unknown, `/theme` prints one line saying so. If the colors look off, pick a dark
theme; set `COLORFGBG` (`15;0` dark, `0;15` light) to tell the TUI which you have.

The skin's `added` and `removed` colors are subtle web backgrounds (dark green, dark red) and turn grey when used as
text, so `/snapshots show` colors diff lines with the skin's `ok` (added) and `error` (removed) text colors, with the
same contrast adjustment. `added`/`removed` are used only if a skin lacks `ok`/`error`.

## Code map

- `src/tui/approvals.{h,cpp}`: approval parsing, prompt, pager and list rendering (`tests/test_tui_approvals.cpp`).
- `src/tui/team.{h,cpp}`: team map parsing, formatting, hand-off notes (`tests/test_tui_team.cpp`).
- `src/tui/snapshots.{h,cpp}`: snapshot parsing, diff statistics, list/diff/preview/result rendering, paging (pure, `tests/test_tui_snapshots.cpp`).
- `src/tui/theme.{h,cpp}`: theme list and lookup, `tui.toml` preference, contrast math, palette from a skin (pure, `tests/test_tui_theme.cpp`).
- `src/tui/tui.cpp`: the command handlers, the y/n revert prompt and the diff pager.
