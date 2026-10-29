# Judas HTTP API (phase 2 clients)

Base: `http://127.0.0.1:<port>` (default 8079). CORS `*` on `/v1` and `/api`.

## State and skins
- `GET /health` -> `{status:"ok", model}`
- `GET /state` -> EngineState: `{model, model_path, loaded, hybrid, max_agents, active, queued, ctx_total, ctx_used, vram_used_mib, vram_total_mib, gen_tok_s, seqs:[{id, tag, phase:"idle|prefill|generate", tokens, priority}], total_prompt_tokens, total_completion_tokens, requests, sessions:[{id,title,updated_ms}], version, vision:bool, ui:{mascot:bool}}`
  (`vision` = the loaded model takes images; `ui.mascot` = the companions setting from `[ui]`)
- `GET /skin.css` -> the active skin as `:root{--bg;--surface;--surface2;--border;--title;--accent;--text;--dim;--ok;--warn;--error;--thinking;--added;--removed;--accent-soft}`. `?name=<skin>` for a specific one.
- `GET /api/skins` -> `{skins:[{name,label,dark,colors:{...}}]}`

## Sessions
- `GET /api/sessions[?limit=200&before=<updated_ms>]` -> `{sessions:[{id,title,cwd,model,mode,created_ms,updated_ms,running}], more}` (newest first; `before` = `updated_ms` of the last row seen pages on; `running` = a turn is in progress)
- `POST /api/sessions` `{title?,cwd?,mode?}` -> session object. `mode` is `chat` (default), `cowork` or `code`: the UI surface the session belongs to, stored on the server. `cwd` empty = the daemon's working directory (the effective folder is stored).
- `PATCH /api/sessions/<id>` `{mode?,title?}` -> session object
- `DELETE /api/sessions/<id>` cancels the chat's agents, waits for them to stop (up to 15 s, else 409), then deletes it
- `GET /api/sessions/<id>[?limit=N&before=<message id>]` -> `{session, agents:[AgentRow], messages:[{id,agent,role,body,created_ms}], more_before, total, turn:{running,event_floor,msg_floor,events_head}}`. Messages cover all agents, oldest first. No `limit` = every message; `limit=N` = the newest N (add `before` for the N before that id). `turn` describes a turn in progress: `event_floor` is the `since` that replays it from its first event, `msg_floor` the last stored message id from before it began.
  - AgentRow: `{id,session,parent,role,task,status:"queued|running|waiting|done|failed|killed",result,created_ms,updated_ms,prompt_tokens,completion_tokens}`
  - message `body` is an OpenAI message object: `{role, content, reasoning_content?, tool_calls?:[{id,type,function:{name,arguments}}], tool_call_id?}`
- `POST /api/sessions/<id>/messages` `{content, cwd?, mode?}` (`<id>`="new" creates one with that mode) -> `{session}`. Starts a turn; watch /events.
- `POST /api/sessions/<id>/cancel` -> `{ok:true}`
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
- `GET /api/events[?since=<id>]` is the global feed (same framing, live-only without `since`): `{type:"session.created"|"session.updated", session:{...}}`, `{type:"session.deleted", id}`, `{type:"turn.started"|"turn.finished", session, status?}`, `{type:"agent.status", session, agent, parent, role, status}`.
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
- `DELETE /api/tasks/<id>` -> `{ok:true}`
A scheduler in the daemon fires due once/interval tasks automatically as new sessions (watch them in Chat/Cowork).

## Heartbeat and channels (phase 4)
- `GET /api/heartbeat` -> `{summary, severity:"ok|notice|warn|critical", facts:{<collector>:{text,ms,error?}}, ran_ms, pushed, enabled, interval_s}`
- `POST /api/heartbeat/run` -> runs it now, returns the same shape
- `GET /api/channels` -> `{channels:[name], default}`
- `POST /api/channels/test` `{channel?}` -> `{ok, error}` (sends a test message to the channel)
