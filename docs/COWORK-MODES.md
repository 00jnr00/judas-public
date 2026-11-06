# Cowork sub-tabs and work modes (contract)

Status: contract for feature/cowork-backend and feature/cowork-ui (2026-11-05). Both branches build on feature/cowork-tabs.

## Decisions (2026-11-05)

- Engineering, Design, Research and Write are sub-tabs of Cowork, not separate rail entries. The Cowork tab's first sub-tab is the plain Cowork mode ("General").
- Form from function: each sub-tab gets a layout built for its job, not the chat column with a different placeholder.
- Every mode works in a project (Judas-made folder under `~/Judas/Projects/<slug>` or an existing folder), shared across Code, Cowork and its sub-tabs.
- Artifacts (`artifact_*`) work in every mode.
- Engineering is strict (units, measured geometry, every number traceable). Design is the loose mode (sketches, concept and cross-section renders, iterations; CAD allowed as a sketch tool).
- Single binary: everything compiles into judas, nothing outside it at runtime.

## Mode ids

Session `mode` values: `cowork`, `engineering`, `design`, `research`, `write` (plus the existing `chat`, `code`, `learn`, `swarm`). The sub-tab is just the session's mode. A session never changes mode.

## Backend (feature/cowork-backend)

Mode validity everywhere modes are listed: `server.cpp valid_mode`, TUI `/mode` choices and check, `agent.cpp` prompt selection, docs/API.md.

Prompts (one function each, like `engineering_prompt()`), short and specific:
- design: loose. Iterate fast; every visible result is an artifact (SVG, HTML, model3d); render after each change with `artifact_render` and look at it; prefer updating the same artifact (new version) over new ones; CAD tools allowed for rough geometry, no precision promises; label sketches as concept.
- research: find and read sources (web_search, web_fetch), record each one used with `source_add`, keep one markdown report artifact for the session current via `artifact_update` as findings land, cite sources as [n] matching the source list, say what is unknown.
- write: the user's text is theirs. Never rewrite it. Suggest with `doc_suggest` only; spelling and clear typos as `spelling`, everything else as `note` (structural feedback, buried lede and so on). The model may create new files the user asks for (outlines, drafts the user requested) but must not edit an existing file in the project.

Tool gating (in the existing gating block in agent.cpp):
- `cad_*`: engineering and design.
- `source_add`: research only, lead only. `web_fetch` URLs are also recorded as sources automatically in research sessions (title from the page if known, `auto: true`).
- `calc_record`: engineering only, lead only.
- `doc_suggest`: write only, lead only. In write mode `edit_file` and any write to an existing file are refused with "Write mode: suggest with doc_suggest; the text is the user's".

New tools:
- `calc_record {name, expr, value, unit, note?}` appends a calc row to the session (engineering calc sheet). Returns the row number.
- `source_add {url, title, quote?, note?}` appends a source (dedupe by url, returns its number n, 1-based, stable).
- `doc_suggest {path, suggestions:[{find, replace, kind:"spelling"|"note", why}]}`: path is relative to the session cwd and must exist. For `spelling`, `find` must occur exactly once in the file at suggest time (else that item is rejected in the tool result with the reason); `replace` is the fixed text. For `note`, `find` anchors the note (exact text, once) and `replace` is empty. Stored per session with ids `g1, g2...`, status `open`.

State is per session, persisted in sessions.db (new tables, ALTER-safe migration like projects).

New endpoints (all JSON; document in docs/API.md):
- `GET /api/sessions/<id>/calcs` -> `{calcs:[{n,name,expr,value,unit,note,created_ms}]}`
- `GET /api/sessions/<id>/sources` -> `{sources:[{n,url,title,quote,note,auto,created_ms}]}`
- `GET /api/sessions/<id>/suggestions` -> `{suggestions:[{id,path,kind,find,replace,why,status,created_ms}]}` (status open|accepted|rejected|stale)
- `POST /api/sessions/<id>/suggestions/<gid>/accept`: for `spelling`, replaces `find` with `replace` in the file only if `find` still occurs exactly once (else status becomes `stale`, 409). Snapshot the file first through the existing snapshot journal so it can be reverted. For `note`, accept just marks it done. -> `{ok, suggestion}`
- `POST /api/sessions/<id>/suggestions/<gid>/reject` -> `{ok, suggestion}`
- `GET /api/sessions/<id>/file?path=REL` -> raw text of a file inside the session cwd (refuse paths outside it, symlinks out of it, files over 2 MB, binary files). Used by the Write document view.
- `PUT /api/sessions/<id>/file?path=REL` body `{text, base_hash}` -> saves the user's own edits from the Write view; 409 if the file changed since `base_hash` (sha256 hex of the text the client loaded; GET returns it in header `X-Judas-Hash` and as `?meta=1` JSON `{text,hash}`).
- `GET /api/sessions/<id>/cad` -> `{available, state}` the current cad_state for that session (same JSON as the tool) or `available:false`.
- `GET /api/sessions/<id>/cad/model.stl` -> binary STL of the current part (404 when none). Used by the Engineering viewport.
- `GET /api/artifacts?session=<id>` filters by originating session (add the param; keep `q`).

Events: the existing session SSE stream gets `{"type":"calcs"}`, `{"type":"sources"}`, `{"type":"suggestions"}`, `{"type":"cad"}` and `{"type":"artifact","id":...}` pings when those change, so layouts refresh without polling. Use whatever event shape the stream already uses; document it.

Tests: unit tests for each tool, gating per mode, suggestion accept (exactly-once, stale, snapshot), file GET/PUT (path escape, hash conflict), migration of an old db. Keep ctest green.

## UI (feature/cowork-ui)

Desktop (web/index.html, web/app.js, web/calm.css, new web/cowork/*.js|css allowed):
- Rail: remove the Engineering entry. Cowork keeps its icon. Inside the Cowork screen, a pill sub-tab row: General, Engineering, Design, Research, Write. Remember the last sub-tab (localStorage, try/catch). Hash routes stay working (`#engineering` opens Cowork > Engineering; add `#design`, `#research`, `#write`).
- Every sub-tab has the project picker (from feature/projects) and the composer.
- Layouts (form from function):
  - General: the current Cowork layout.
  - Engineering: calc sheet + 3D viewport. Left: chat column (narrower). Right, split: top a 3D viewport of `/cad/model.stl` (reuse web/vendor/artifacts/viewer3d.js), bottom a calc sheet table (name, expression, value, unit, note) from `/calcs`, plus a small CAD state strip (part, features, volume). Refresh on the SSE pings.
  - Design: canvas first. The current design artifact fills the main area (the session's most recent artifact; picker if several), a version filmstrip along the bottom (thumbnails of versions, click to view, compare two side by side), a reference board strip (images attached in the session and other artifacts the user pins to it), chat as a collapsible side panel.
  - Research: sources pane + live report. Left: numbered sources list (title, host, quote, auto badge, open link). Center: the session's report artifact rendered as markdown (web/vendor/artifacts/mdview.js), live updating; [n] citations link to the source rows. Chat as a narrow right column or bottom drawer.
  - Write: document first. The document (choose a file in the project, or New document) fills the page in an editor the user types in (plain textarea or contenteditable plain text, saves via PUT with hash check, autosave with debounce, conflict banner). Suggestions show as margin notes aligned to their anchor text: spelling ones with Accept and Reject, notes with Done and Dismiss. Underline the anchor text in the document. Chat is a margin thread, not a main column. Never auto-apply anything.
- Light and dark skins and all existing skins keep working (use the calm.css tokens).

Phone (web/m/*):
- The Cowork tab gets the same sub-tab pills. Layouts collapse to one column with a segmented switch at the top of a session: Engineering (Chat | Model | Calcs), Design (Canvas | Versions | Chat), Research (Report | Sources | Chat), Write (Document | Notes | Chat).

Tests: extend tests/e2e (fake model) with one test per sub-tab that opens it, starts a session in a project and sees its layout. Use stubbed endpoints only when the backend branch is absent; the final merge runs them against the real binary.
