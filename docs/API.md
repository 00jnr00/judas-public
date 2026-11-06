# Judas HTTP API (phase 2 clients)

Base: `http://127.0.0.1:<port>` (default 8079). CORS `*` on `/v1` and `/api`.

## State and skins
- `GET /health` -> `{status:"ok", model}`
- `GET /state` -> EngineState: `{model, model_path, loaded, hybrid, max_agents, active, queued, ctx_total, ctx_used, vram_used_mib, vram_total_mib, gen_tok_s, seqs:[{id, tag, phase:"idle|prefill|generate", tokens, priority}], total_prompt_tokens, total_completion_tokens, requests, sessions:[{id,title,updated_ms}], version, vision:bool, ui:{mascot:bool}}`
  (`vision` = the loaded model takes images; `ui.mascot` = the companions setting from `[ui]`)
- `GET /skin.css` -> the active skin as `:root{--bg;--surface;--surface2;--border;--title;--accent;--text;--dim;--ok;--warn;--error;--thinking;--added;--removed;--accent-soft}`. `?name=<skin>` for a specific one.
- `GET /api/skins` -> `{skins:[{name,label,dark,colors:{...},tokens:{...}}]}`
- `GET /api/skins/<name>` -> `{name,label,dark,vars:{"--bg":...}}` (`current` = the server's active skin); see `docs/THEMES.md`

## Sessions
- `GET /api/sessions[?limit=200&before=<updated_ms>&project=<project id>]` -> `{sessions:[{id,title,cwd,model,mode,created_ms,updated_ms,running}], more}` (newest first; `before` = `updated_ms` of the last row seen pages on; `running` = a turn is in progress)
- `POST /api/sessions` `{title?,cwd?,mode?,project?}` -> session object (`project_id` is on every session object; list rows also carry `project_name`). With `project` (a project id) the project's folder becomes the cwd and `cwd` is ignored. `mode` is `chat` (default), `cowork`, `code`, `learn`, `swarm`, `engineering`, `design`, `research` or `write`: the UI surface the session belongs to, stored on the server. `cwd` empty = the daemon's working directory (the effective folder is stored).
- `PATCH /api/sessions/<id>` `{mode?,title?,project?}` -> session object. `project` = a project id attaches (the folder becomes the cwd, the instructions apply from the next turn), `""` detaches (the cwd stays). 404 for an unknown project, 409 while a turn is running. `mode` moves only between chat, cowork, code, learn and swarm; an engineering, design, research or write session keeps its mode (409), and a message carrying another mode for it is run in its own mode.
- `DELETE /api/sessions/<id>` cancels the chat's agents, waits for them to stop (up to 15 s, else 409), then deletes it
- `GET /api/sessions/<id>[?limit=N&before=<message id>]` -> `{session, agents:[AgentRow], messages:[{id,agent,role,body,created_ms}], more_before, total, turn:{running,event_floor,msg_floor,events_head}}`. Messages cover all agents, oldest first. No `limit` = every message; `limit=N` = the newest N (add `before` for the N before that id). `turn` describes a turn in progress: `event_floor` is the `since` that replays it from its first event, `msg_floor` the last stored message id from before it began.
  - AgentRow: `{id,session,parent,role,task,status:"queued|running|waiting|done|failed|killed",result,created_ms,updated_ms,prompt_tokens,completion_tokens}`
  - message `body` is an OpenAI message object: `{role, content, reasoning_content?, tool_calls?:[{id,type,function:{name,arguments}}], tool_call_id?}`
- `POST /api/sessions/<id>/messages` `{content, cwd?, mode?, project?}` (`<id>`="new" creates one with that mode; `project` = a project id, an alternative to `cwd`: the project's folder is used, and an existing session is attached to the project) -> `{session}`. Starts a turn; watch /events. If the lead is busy the message is queued instead (next bullets).
- Interrupt: `POST /api/sessions/<id>/messages` with `interrupt:true` stops the running turn (every agent of the session; queued messages are kept) and sends this message as the next turn as soon as the lead has stopped -> `{session, interrupted:true}`. With nothing running it is a plain send. `POST /api/sessions/<id>/queue/<qid>/send` does the same with one queued message (it leaves the queue; the rest stay) -> `{ok:true, interrupted:true, queue}`, 404 when it already started.
- `POST /api/sessions/<id>/cancel` -> `{ok:true}`. Stops every agent of the session and drops its queued messages (a stop means stop; resend what you still want). A pause (`/agents/<id>/pause` on the lead) keeps the queue.
- Queued messages: `POST /api/sessions/<id>/messages` while the lead is mid-turn does not start a second turn and is not rejected: the message waits in a per-session FIFO queue (cap 10) and the reply is `{session, queued:true, id, queue:[{id,text}]}`. When the lead's turn ends (done or failed; not paused, not cancelled) the next message starts its own turn, one turn per message (never merged), announced by a lead `spawned` event with `from_queue:true`; `wait_session` and the busy state hold until the queue is empty. An 11th message gets 409 `{error:{type:"conflict"}}` and nothing is lost. The queue lives in the daemon: it survives a page reload and works the same from the API and the TUI, but not a daemon restart. It is visible as `queued:[{id,text}]` in `GET /api/sessions` rows, in the `session` object and `turn` of `GET /api/sessions/<id>`, and in `GET /api/sessions/<id>/queue`. `text` is what the user typed (attachment blocks are appended when it is sent). Every change emits a lead `queue` event `{queued:[{id,text}]}` on the session stream.
- `DELETE /api/sessions/<id>/queue/<qid>` -> `{ok, queue}` removes a queued message (404 once it has started). `PATCH /api/sessions/<id>/queue/<qid>` `{text}` -> `{ok, queue}` edits it in place, keeping its position.
- `POST /api/sessions/<id>/recover` -> `{ok,restored,removed,conflicts,irreversible,error}` (undo every change this chat made; cancels it first)
- `POST /api/sessions/<id>/seal` -> `{ok:true, freed_bytes}` (keep the current result, drop this chat's undo history to save disk, end the chat)

## Attachments and vision
Files users attach in the chat bar (web and TUI). Storage and extraction rules: [ATTACHMENTS.md](ATTACHMENTS.md).
- `POST /api/attachments?name=<filename>` body = raw file bytes (`Content-Type` = the file's mime type). Also accepts `multipart/form-data` with a field `file`. Returns the attachment:
  `{"id","name","mime","kind","bytes","text_chars","truncated","pages":[ids],"url":"/api/attachments/ID"}`. `kind` is `image`, `pdf`, `doc` or `text`; `pages` lists the page images of a scanned PDF. 400 on an empty body, 413 over `attachments.max_bytes`. The same bytes always get the same `id`.
- `GET /api/attachments/<id>` -> the original bytes, inline, with their mime type. `GET /api/attachments/<id>?meta=1` -> the attachment JSON above.
- `POST /api/sessions/<id|new>/messages` takes an optional `"attachments": ["ID", ...]` next to `content`. The server appends each attachment's block to the message text, so `content` may be empty when attachments are present. A new session sent with empty text is titled after the first attachment's name.
- `POST /api/screenshot` `{"region": false}` -> takes a screenshot of the host screen with `grim` (`region: true` lets you select an area with `slurp`), stores it as an attachment and returns its attachment JSON. 501 when `grim` is not installed.

Images reach a model only when it has vision (`vision` in `/api/state`). Without it, the image is described by its OCR text instead.

## Learning
- `POST /api/learn` `{request?, session?, cwd?}` -> `{session}`. Rewrites the request to the same `[/learn]` prompt the TUI uses and starts it as a normal turn; watch the session's events like any other turn. An empty or missing `request` reviews the current conversation. With `session` it appends to that session; without it a new chat session is created. `cwd` is the working folder for a new session and the fallback for an existing one. Typing `/learn <request>` in the TUI or the web chat does the same.

## Live events (SSE)
`GET /api/sessions/<id>/events` streams `id: <n>\ndata: <json>\n\n` until the lead agent's turn ends, then closes. A `: ping` comment every 15 s. Each event: `{type, session, agent, parent, role, data}`.
- Ids rise by one per session (reset when the daemon restarts). The daemon keeps a bounded ring of recent events per session (20000 events or 8 MiB, newest 16 sessions), so a client can resume: `?since=<id>` or the `Last-Event-ID` header replays everything after that id. With neither, a turn in progress is replayed from its first event (a refreshed page rebuilds the partial message), then the stream goes live. A `{type:"gap"}` frame means older events were evicted (or the daemon restarted).
- `?follow=1` keeps the stream open across turns (a turn started by another client, a scheduled run or curl shows up on it); a deleted session ends it with `{type:"session_deleted"}`. Without `follow` the stream ends at the lead's first `done`, as before.
- `GET /api/events[?since=<id>]` is the global feed (same framing, live-only without `since`): `{type:"session.created"|"session.updated", session:{...}}` (also raised for sessions created by `POST /api/sessions/new/messages`, with the requested `mode`, so scripts and bridges show up in the desktop and phone lists without a reload), `{type:"session.deleted", id}`, `{type:"turn.started"|"turn.finished", session, status?}`, `{type:"agent.status", session, agent, parent, role, status}`.
- Each open stream holds one HTTP worker thread. The pool grows on demand up to `JUDAS_HTTP_THREADS` (default 512, minimum 32) per listener.
- Extra binds (`server.extra_binds`, e.g. a tailnet IP) that fail at start do not stop the daemon: they are retried every 3 s until the address exists. Only `server.bind` must succeed.
- `spawned` data `{task, role}` (parent empty = lead)
- `delta` data `{text}` (visible answer chunk, append)
- `reasoning` data `{text}` (thinking chunk)
- `tool_call` data `{id, name, args}`
- `tool_result` data `{id, name, ok, output, checkpoint}`
- `message` data `{message:<oai message>, usage:{prompt,cached,completion,prefill_s,generate_s}}`
- `status` data `{error?}`
- `done` data `{result, status}` (parent empty = the whole turn is finished)
- Cowork pings (same framing, `data` is `{}` unless noted; a client refetches the matching endpoint): `calcs` (a calc row was added), `sources` (added or auto-recorded), `suggestions` (a doc_suggest call, or a user accept/reject), `cad` (the live CAD model changed), `artifact` with `data:{id}` (an artifact was created or got a new version). See Cowork work modes.
- `approval_request` data `{id, tool, what, strict, created_ms}` (an agent is blocked waiting for the user's yes; see Approvals)
- `approval_resolved` data `{id, approved, by}` (`by`: `user`, `cancelled`, `timeout` or `shutdown`)

## Approvals
When a tool needs the user's yes (permission mode `suggest` asks for writes and shell, `auto-edit` for shell, MCP and skill actions that ask), the agent blocks and the daemon raises a pending approval. Any client answers it; the first answer wins and resolves it for everyone. A cancelled agent, a daemon shutdown or the timeout (600 s) resolves it as declined, so a session with no client attached never hangs for good.
- `GET /api/sessions/<id>/approvals` -> `{approvals:[{id, agent, role, tool, what, strict, created_ms}]}` (pending only, oldest first). `what` is the text to show: the action plus a diff (`-` old lines, `+` new lines) for `edit_file` and `write_file`, the dry run for the planner tools.
- `POST /api/sessions/<id>/approvals/<aid>` `{"approve": true|false}` -> `{ok, id, approved}`. 404 when the approval is unknown, 409 when it was already resolved, 400 on a bad body.
- The session stream carries `approval_request` when one is raised and `approval_resolved` when it ends. The global feed carries `approval.requested` and `approval.resolved` (same data plus `session`, `agent`).
- `strict` approvals (`approve_strict`: the mach12 planner tools) are never answered by the permission mode: `full-auto` still asks. Everything else is auto-approved in `full-auto` and never raised.
- The safety floor (sudo, su, doas, pkexec, passwd, faillock, auth databases) is refused before any approval is raised; no answer can unlock it.

## Snapshots (the undo timeline)
- `GET /api/snapshots?session=&limit=` -> `{snapshots:[{id,session,agent,label,created_ms,committed,reverted,files:[{path,before_hash,after_hash}],irreversible:[...]}]}`
- `GET /api/snapshots/<id>/diff` -> `text/plain` unified diff
- `POST /api/snapshots/<id>/revert` `{to?:bool}` -> `{ok,restored:[],removed:[],conflicts:[],irreversible:[],error}`

## OpenAI-compatible
- `GET /v1/models`, `POST /v1/chat/completions` (stream + non-stream, tools, `chat_template_kwargs.enable_thinking`).
- Message `content` may be an array of parts: `{"type":"text","text":...}` and `{"type":"image_url","image_url":{"url":"data:image/png;base64,..."}}`. Only `data:` URLs are read; `http(s)` URLs are ignored. Images need a model with vision (see `vision` in `/api/state`).

## Scheduled tasks (the Tasks surface)
- `GET /api/tasks` -> `{tasks:[{id,title,prompt,cwd,kind,run_at_ms,interval_s,next_ms,enabled,last_session,last_run_ms,created_ms}]}`
  - kind: "manual" (run on demand only), "once" (fire at run_at_ms, then disables), "interval" (every interval_s seconds)
- `POST /api/tasks` `{prompt, title?, cwd?, kind?, run_at_ms?, interval_s?, enabled?}` -> the task
- `POST /api/tasks/<id>/run` -> `{session, task}` (runs it now as a new session)
- `POST /api/tasks/<id>/toggle` -> the task (flips enabled)
- `PATCH /api/tasks/<id>` `{title?, prompt?, cwd?, kind?, run_at_ms?, interval_s?, enabled?}` -> the task. Fields left out keep their values; the next run is recomputed when the schedule changes (an interval steps forward from run_at_ms). 400 for an empty prompt, an unknown kind, a once time in the past or an interval under 60 s; 404 unknown id.
- `DELETE /api/tasks/<id>` -> `{ok:true}`
A scheduler in the daemon fires due once/interval tasks automatically as new sessions (watch them in Chat/Cowork).

## Heartbeat and channels (phase 4)
- `GET /api/heartbeat` -> `{summary, severity:"ok|notice|warn|critical", facts:{<collector>:{text,ms,error?}}, ran_ms, pushed, enabled, interval_s}`
- `POST /api/heartbeat/run` -> runs it now, returns the same shape
- `GET /api/channels` -> `{channels:[name], default}`
- `POST /api/channels/test` `{channel?}` -> `{ok, error}` (sends a test message to the channel)

## Swarm teams (docs/SWARM-TEAMS.md)
- `GET /api/sessions/<id>/team` -> `{session, lead:AgentLive|null, teams:[Team], agents:[AgentLive]}` (in memory; empty `teams` after a daemon restart)
  - Team: `{name, brief, rules:[str], lead_member:str|"", owner:agent_id, disbanded:bool, members:[{id, role, count, model, skills:[str]}], edges:[{from,to,kind}], copies:[{copy, member, agent:agent_id|""}], place:[[copy,...],...], map:str, warnings:[str]}`
    - `edges` holds only the extra spec edges; the default edges (lead -> team lead or every member, team lead -> members) are implied.
    - `place` is the pinned layout from the map file: row 1 first, each row a list of copies left to right. Empty = automatic layout. Copies not in any row go in an extra last row.
    - `map` is the team in the canonical map format (docs/SWARM-TEAMS.md "Map files"); `warnings` lists map lines that were skipped.
- `POST /api/sessions/<id>/teams/<team>/save` body `{name, note?, overwrite?}` -> `{ok:true, path}`. Writes the team's map (with a comment header: date, session, outcome note, run stats) to `~/.config/judas/teams/<name>.map`. `name` is `[a-z0-9-]`, 1 to 40. 400 on a bad name or unknown team, 409 when the file exists and `overwrite` is not true; with `overwrite` the old file is kept as `<name>.map.bak`.
- `GET /api/teams/saved` -> `[{name, title, date, outcome}]` for every `.map` and `.toml` in the teams folder (`title` = first comment line, `date` and `outcome` from the header; empty for hand-written files).
  - AgentLive: AgentRow plus `{team:str|"", member:str|"", activity:"queued|prefilling|generating|tool|waiting|blocked|done|failed|idle", tool:str|"", depth, model:"gpu|cpu", tok_s:number, ctx_pct:number|null, last_output:str}` (`ctx_pct` = the last prompt's share of this agent's context cap, 0 to 100) (`last_output` = tail of the latest assistant text, at most 600 chars)
- `POST /api/sessions/<id>/agents/<agent>/cancel` -> `{ok}` (stops that agent's turn, status killed; its queued input is dropped)
- `POST /api/sessions/<id>/agents/<agent>/pause` -> `{ok}` (stops the turn, status paused, resumable)
- `POST /api/sessions/<id>/agents/<agent>/resume` -> `{ok}` (a paused or killed agent carries on with its task)
- Events on the session stream (in addition to the ones above):
  - `activity` data `{activity, tool?}` whenever an agent's activity changes
  - `team` data `{action:"created"|"assigned"|"disbanded"|"message"|"report", team, member?, copy?, agent?, from?, to?}`. `from` and `to` are copies or `lead`: assigned goes from the assigner to the copy, message along its edge, report from a member whose turn ended to its parent. The UI animates a hand-off along that edge.
  - `spawned` data gains `{team?, member?, model?}` for team members

## Cowork work modes (docs/COWORK-MODES.md)
Session `mode` values: `chat`, `cowork`, `code`, `learn`, `swarm`, `engineering`, `design`, `research`, `write` (anything else is a 400). `engineering`, `design`, `research` and `write` are the Cowork sub-tabs; each adds a short prompt to every agent of the session.
- Tool gating: `cad_*` in engineering and design; `calc_record` engineering (lead only); `source_add` research (lead only); `doc_suggest` write (lead only). In write mode `edit_file` and any `write_file` over an existing file are refused with "Write mode: suggest with doc_suggest; the text is the user's" (new files are allowed). In research mode every `web_fetch` URL is also recorded as a source (`auto:true`).
- Tools: `calc_record {name, expr, value, unit, note?}` appends a calc row and returns its number. `source_add {url, title, quote?, note?}` appends a source and returns its stable 1-based number n (the same url keeps its number; a later call fills in an auto row's title, quote and note). `doc_suggest {path, suggestions:[{find, replace, kind:"spelling"|"note", why}]}`: `path` is relative to the session folder and must exist; `find` must occur exactly once in the file; items that fail are rejected one by one in the tool result; stored as `g1, g2, ...` with status `open`.
- `GET /api/sessions/<id>/calcs` -> `{calcs:[{n,name,expr,value,unit,note,created_ms}]}`
- `GET /api/sessions/<id>/sources` -> `{sources:[{n,url,title,quote,note,auto,created_ms}]}`
- `GET /api/sessions/<id>/suggestions` -> `{suggestions:[{id,path,kind,find,replace,why,status,created_ms}]}`; status is `open`, `accepted`, `rejected` or `stale`.
- `POST /api/sessions/<id>/suggestions/<gid>/accept` -> `{ok, suggestion, checkpoint?}`. `spelling`: replaces `find` with `replace` only if `find` still occurs exactly once; the file is recorded in the snapshot journal first (`checkpoint`, revertible from Snapshots). Otherwise the suggestion becomes `stale` and the reply is 409 `{error, suggestion}`. `note`: marks it accepted. Only `open` suggestions can be accepted (409 otherwise); unknown id 404.
- `POST /api/sessions/<id>/suggestions/<gid>/reject` -> `{ok, suggestion}` (409 when not open).
- `GET /api/sessions/<id>/files` -> `{files:[{path, mtime_ms}]}`: text documents (.md .markdown .txt .tex .rst .org .adoc .html) in the session folder, newest first, at most 300, four folders deep, skipping hidden folders, node_modules, build output and symlinks. Feeds the Write file picker.
- `GET /api/sessions/<id>/file?path=REL` -> the raw text of a file inside the session folder (`text/plain`), hash in header `X-Judas-Hash` (sha256 hex of the text). `&meta=1` -> `{text, hash}`. 403 for absolute paths, `..` escapes and symlinks that leave the folder, 404 missing, 413 over 2 MB, 415 binary.
- `PUT /api/sessions/<id>/file?path=REL` `{text, base_hash}` -> `{ok, hash}`. Saves the user's own edit atomically. 409 `{error, hash}` when the file's current hash is not `base_hash` (`hash` is the current one); a missing file is created when `base_hash` is empty. Same limits as GET.
- `GET /api/sessions/<id>/cad` -> `{available, state}` (`state` is the cad_state JSON of the process's one live model) or `{available:false, why}` when no kernel is compiled in. `GET /api/sessions/<id>/cad/model.stl` -> binary STL of the current part, 404 when there is none.
- `GET /api/artifacts?session=<id>` lists only artifacts created by that session (combines with `q`).
- MCP clients get 403 on these endpoints for health-tainted sessions, like the other session reads.

## Projects

A project is a name, a folder and optional instructions. Sessions attach to a project (`project` on create, on a message, or `PATCH /api/sessions/<id>`); the folder becomes their cwd and the instructions are added to the system prompt of every agent in the session, in every mode (Chat, Cowork, Code, Engineering and the rest). Two kinds: `existing` (any absolute folder the user picked; nothing is created) and `made` (a new folder Judas creates under `[projects] root`, default `~/Judas/Projects/<slug>/`, `-2`, `-3` ... on a name collision; recreated if it vanishes). Judas never deletes a project folder by itself.

- `GET /api/projects` -> `{projects:[{id,name,folder,instructions,kind,folder_exists,session_count,created_ms,updated_ms}], root}`
- `POST /api/projects` `{name, mode?:"make"|"existing", folder?, instructions?}` -> project. `mode` defaults to `existing` when `folder` is given, else `make`. 400 for an empty name, a relative or missing folder.
- `GET /api/projects/<id>` -> project plus `sessions:[{id,title,mode,updated_ms}]`
- `PATCH /api/projects/<id>` `{name?, instructions?, folder?}` -> project. `folder` only for `existing` projects. Instructions are capped at 8000 characters. Sessions of the project that are not running pick the change up at once.
- `DELETE /api/projects/<id>` -> `{ok, folder_removed, warning}`. Removes the record only; its sessions stay, detached. `?delete_folder=1&confirm_folder=1` (both, or 400) also removes the folder recursively, refused for the home folder, `/`, and top-level system folders.
- Lead tools (all modes): `project_info` (this chat's project: name, folder, instructions) and `project_list`.
- Config: `[projects] root = "~/Judas/Projects"`.

## Location

The user's position comes from the device he is on: the Geolocation API in the desktop and phone pages, and the WebView in the Android app. Settings has "Share my location with Judas" (off by default; turning it on triggers the browser or Android permission prompt). While it is on, open Judas pages report a fix when it changes meaningfully (more than about 100 m) and at least every 5 minutes while the page is visible.

- `POST /api/location` `{lat, lon, accuracy?, timestamp?, device?}` -> status. `{sharing:false}` or `DELETE /api/location` clears the fix.
- `GET /api/location` -> `{sharing, has_fix, age_s?, stale?, device?, place?}` (never coordinates).
- Storage: the server keeps only the latest fix, in memory, with the time it was received. Nothing is written to disk, nothing is logged (the access log has paths only). A fix older than 10 minutes is flagged stale; older than 6 hours it is forgotten. A restart forgets it.
- Lead tool `location_get` (all modes): the fix, its age, the device and the nearest place from an offline reverse geocoder (GeoNames cities with population 15,000 or more, CC BY 4.0, compiled into the binary; `LICENSES/GEONAMES.md`). With nothing shared it tells the model to ask the user to enable the toggle.
- Privacy decision: location is not "health-tainted" (web search and MCP stay available, so "what is open near me" works), but a session that calls `location_get` is marked `location_used` for good: `memory_append` and `skill_manage` are refused in it, and background review skips it, so a position cannot be saved into memory, skills or learned lessons. The position itself is never persisted; the chat history keeps the tool result like any other message.

## Health Link (desktop side)

Read-only pull of the user's Health Connect data from the phone (Judas Android app, port 8765, bearer token) over the tailnet. Off unless `[health] enabled = true`. Data stays on the phone and is fetched per question.

```toml
[health]
enabled = true
url = "http://moto-g-5g---2024:8765"
token_env = "JUDAS_HEALTH_TOKEN"   # env var NAME; the value lives in secrets.env
timeout_s = 8
weekly_checkin = true
weekly_day = "sun"                 # mon..fri or sun; "sat" is rejected at load (Saturday is rest day)
weekly_hour = 19                   # local hour 0..23
```

A project `.judas/config.toml` cannot set `[health]` (untrusted-file rule). The weekly message uses `[heartbeat] quiet_hours` and the heartbeat/default channel.

- Tool `health_query(metric, from, to, granularity)` (lead agent, read only). `metric` is `sleep`, `heart_rate`, `resting_hr`, `hrv`, `steps` or `week_vs_normal`. `week_vs_normal` is computed in code: this week (7 days) against the 28 days before it, as averages, deltas and day counts, no verdicts. Errors are plain: phone not reachable, token rejected (401), permission not granted for the metric (403), Health Connect unavailable (503). Refused when the active model is a remote non-private endpoint.
- Weekly check-in: on `weekly_day` at or after `weekly_hour` (never Saturday, never in quiet hours), one message only if sleep is 60 min or more under normal, resting HR 5 bpm or more over, or HRV 15% or more under, with at least 4 days this week and 14 in the normal window for that metric. Silent when the phone is unreachable (retried every 30 min that day) or the week is normal. The last run date is kept in `<data_dir>/health-state.json`, so a restart never sends twice. Log lines carry no health numbers.
- `GET /api/health/status` -> `{enabled, configured, token_set, url, reachable, granted, weekly_checkin, weekly_day, weekly_hour, health_connect?, background_read?, error?}`.
- `POST /api/health/weekly {"enabled":bool}` -> `{ok, weekly_checkin, live}`; rewrites `[health].weekly_checkin` in `config.toml` (rest of the file kept) and flips the live switch.
- Taint: a session whose `health_query` returned data is marked `health_tainted` in `sessions.db` for good. In it the registry refuses `memory_append`, `skill_manage`, `web_search`, `web_fetch` and every MCP tool; `show_image` will not fetch URLs, `notify` will not use ntfy channels, reply images are not fetched, `shell` runs only inside bwrap with the network off (refused when bwrap is missing), and `session_search` from other chats never returns tainted sessions. Background review skips tainted sessions. The model sees: "health data is in this chat, so tools that send anything off this machine are off for this session".

Judas MCP and tainted sessions: `session_json` carries `health_tainted`. Requests with the header `X-Judas-Client: mcp` (sent by `tools/judas-mcp`) get no content for a tainted session: empty messages, agent task and result, queued text and turn reason blanked, title and list titles replaced with "this session has health data; open it in the Judas app", search hits dropped, `/api/sessions/<id>/events` and `/api/events` answer 403. `judas-mcp` also refuses on its own (status, result and wait return non-content fields plus `content_withheld`; `judas_message` does not send into a tainted session). The web UI and TUI send no such header and are unchanged.
