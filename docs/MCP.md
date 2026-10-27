# MCP client

Judas can take tools from MCP servers (Model Context Protocol, stdio transport) instead of having every tool compiled in. A server is a child process; its tools show up in the registry as ordinary tools, so the agent loop, hooks, loop guard, journal and permission modes treat them like `read_file` or `shell`.

Code: `src/mcp/` (`client` = one server process, `manager` = servers to registry, `launch` = argv, env and sandbox, `filter` = role filters, `schema` = argument validation). Tests: `tests/test_mcp_*.cpp` with `tests/fake_mcp_server.py`; a manual smoke test against a real server: `tests/live_mcp.cpp`.

## Adding a server

In `~/.config/judas/config.toml` (MCP servers are read from the user file only, see "Safety model"):

```toml
[mcp]
max_tools_per_agent = 12     # MCP tools one agent sees; 0 = no cap

[mcp.servers.docs]           # Microsoft markitdown-mcp: PDF, DOCX, XLSX, PPTX, HTML -> Markdown
command = ["~/.local/bin/markitdown-mcp"]
timeout_s = 120              # per tool call
trusted = true               # sandboxed read-only server: skip the per-call approval prompt
# network = false            # default; per server: no network inside its sandbox unless this is true

[mcp.servers.pdf]            # mcp-pdf: tables, forms, OCR (54 tools; keep the ones you want)
command = ["~/.local/bin/mcp-pdf"]
tools_allow = ["tableextraction__extract_tables", "formmanagement__extract_form_data",
               "formmanagement__fill_form_pdf", "textextraction__extract_text"]
prefix = "pdf__"
writable = ["~/Documents/filled"]   # fill_form_pdf writes a new PDF here; everything else is read-only
```

Restart `judas.service` after editing. Tools appear as `docs__convert_to_markdown`, `pdf__formmanagement__fill_form_pdf`, and so on. Status lines go to stderr (`mcp[docs]: failed to start: ...`).

Per-server keys:

| key | default | meaning |
|---|---|---|
| `command` | required | argv. `command[0]` is looked up in PATH, then `~/.local/bin`, `/usr/local/bin`. `~` and `${VAR}` expand. |
| `env` | `{}` | extra environment. Nothing else is inherited except PATH, HOME, USER, LOGNAME, LANG, LC_ALL, TERM. |
| `cwd` | daemon's | working directory |
| `enabled` | `true` | |
| `timeout_s` | 60 | per `tools/call`; on expiry the call is cancelled (`notifications/cancelled`) |
| `startup_timeout_s` | 30 | spawn + handshake + first `tools/list` |
| `tools_allow` / `tools_deny` | all / none | globs (`*`, `?`) on the server's own tool names; deny wins |
| `prefix` | `"<name>__"` | registry name prefix. Characters outside `[A-Za-z0-9_-]` become `_`. |
| `sandbox` | `true` | run under bwrap (see below) |
| `network` | `false` | network inside the sandbox |
| `writable` | `[]` | paths the sandboxed server may write |
| `trusted` | `false` | skip the approval prompt (never skips the safety floor) |
| `protocol` | `"auto"` | `auto`, `legacy` (initialize handshake), `modern` (2026-07-28) |

`judas config show` prints the effective `[mcp]` section. At startup the daemon reports config problems (a missing `command`, a bad `protocol`, a negative cap) on stderr.

## Protocol

The latest revision is 2026-07-28 (stateless: no `initialize`, every request carries the version and capabilities in `_meta`, `server/discover` replaces the handshake). Servers built before it speak 2025-11-25 and earlier (`initialize` / `notifications/initialized`). In `auto` mode the client does what the stdio binding recommends: probe with `server/discover`; a modern answer (or a modern error such as `UnsupportedProtocolVersionError`) selects the modern era; any other answer or silence for 4 s selects the legacy handshake (2025-11-25, accepting 2025-06-18, 2025-03-26, 2024-11-05 from the server). Set `protocol = "legacy"` for a server that is slow to boot and does not answer unknown methods quickly.

Implemented: `tools/list` with `nextCursor` pagination, `tools/call`, `notifications/tools/list_changed` (legacy era; the registry refreshes within a moment), cancellation on timeout, ping answered, shutdown by closing stdin then SIGTERM then SIGKILL on the whole process group. Not offered to servers: roots, sampling, elicitation, tasks, resources and prompts as model-visible features. A server that answers `tools/call` with `input_required` (multi round-trip) gets an error back.

In the modern era, `list_changed` needs a `subscriptions/listen` stream, which this client does not open; the tool list is read at start and again after a restart. (Unverified against a server that changes its list at runtime.)

Crash handling: if the process dies, the call in flight fails with the last stderr lines and the next call restarts it. Restarts after a failed call back off exponentially (0.5 s, 1 s, 2 s, ... capped at 30 s); a call made inside the window fails at once with "will be restarted on a call after Ns". A server that exits while idle is restarted by the next call without delay. After a restart the tool list is re-read.

Debugging: `JUDAS_MCP_DEBUG=1` logs each message (size and method, never content) and the server's stderr.

## Tool cap and role filters

Small local models degrade with many tools, so one agent sees at most `max_tools_per_agent` (default 12) MCP tools, taken in registry order (servers alphabetical, then the server's own order). Use `tools_allow` to pick which ones; a server with 54 tools is not 54 prompt entries. The cap counts after role filtering.

By default MCP tools go to every role (lead, workers, helpers). A role's `tools:` frontmatter is a whitelist as before, and understands:

| entry | meaning |
|---|---|
| `read_file`, `docs__*` | a tool name or glob |
| `mcp` or `mcp:*` | every MCP tool |
| `mcp:<server>` | every tool of that server |
| `!<entry>` | exclude, e.g. `!mcp:*`, `!pdf__formmanagement__*`, `!shell` |

A role with no list sees everything not excluded. A role with a whitelist does not see MCP tools unless an entry includes them (the built-in `reviewer` is read-only on purpose). Exclusion-only lists work: `tools: !mcp:*` means "everything except MCP tools".

```markdown
---
name: docs-clerk
tools: read_file, list_dir, grep, mcp:docs, mcp:pdf
---
```

## Results

Content blocks become text for the model: `text` as is; `resource` text inlined with its URI; `resource_link` as a one-line note; `image` and `audio` are saved to `<data_dir>/mcp-files/<session>/` and replaced by `[image: image/png, 8 bytes, saved to <path>]`; `structuredContent` is used when there is no text. `isError` becomes a failed tool result. Output longer than `[mcp] max_result_chars` (30000) keeps its head and tail, like the shell tool. Descriptions are trimmed to the first paragraph, 300 characters; the input schema is passed through unchanged (minus `$schema`).

## Safety model

Third-party servers are untrusted code that the model can talk to.

1. **Sandbox.** When `permissions.sandbox = "bwrap"` and `bwrap` is on PATH, each server runs under bubblewrap: root read-only, a fresh `/tmp`, `~/.cache` replaced by a throwaway tmpfs, `permissions.secret_paths` (default `~/.ssh`, `~/.gnupg`), `~/.config/judas` and the data dir hidden, pid/uts/ipc/cgroup namespaces, and network off (per server: `network = true` turns it on for that server alone). Only paths in `writable` are writable. Network is per server, never global.
2. **Fail closed.** A server that should be sandboxed (`sandbox = true`, the default) but cannot be, because `/usr/bin/bwrap` is missing or `permissions.sandbox = "none"`, is refused: it does not start, one stderr line says why, and `Manager::status()` carries the reason in the sandbox note. It starts unconfined only when `permissions.allow_unsandboxed_mcp = true` (an explicit opt-in), or when the server itself set `sandbox = false` (a deliberate opt-out for a server you trust, so it is not gated by that flag). Startup also prints one warning when `permissions.sandbox = "none"`: nothing on the machine is confined then.
3. **Scrubbed environment.** The server gets PATH, HOME and a few locale variables plus its `env` table, not the daemon's environment (so API keys in it do not leak).
4. **Safety floor.** Before any call reaches a server, in every permission mode including `full-auto`: string arguments under command-like keys (`command`, `cmd`, `cmdline`, `shell`, `script`, `argv`) go through the shell tool's block (sudo, su, doas, passwd, faillock, writes to the auth databases); any argument shaped like a path (`/...`, `~...`, `./`, `../`, `file://`) is resolved first (~ expanded, made absolute against the working directory, `weakly_canonical`, so symlinks and `..` resolve) and both the given and the resolved path are checked against the secret-path list (`permissions.secret_paths`, a `.ssh` or `.gnupg` directory anywhere in the path, `.env`, `id_rsa*`, `id_ed25519*`, `*.pem`). So a symlink pointing into a secret directory and an `a/../.ssh/id_rsa` spelling are refused. A refusal is returned as the tool result, so the model sees it and the agents are told to report walls rather than escalate.
5. **Approval.** A tool without `readOnlyHint: true` is treated like the shell: `suggest` and `auto-edit` ask the approver, `full-auto` does not; for such a tool path-like arguments must also pass the write policy (`permissions.allow_paths` / `deny_paths`), even when the path is only an input. `trusted = true` on a server skips the prompt, not the floor. Annotations from a server are untrusted hints; a server lying about `readOnlyHint` can only skip the prompt, and the sandbox still holds.
6. **Same path as built-ins.** MCP calls go through the same agent-loop code as built-in tools: `pre-tool` / `post-tool` hooks (match on the prefixed name), loop guard, journal checkpoint (non-read-only calls are noted as irreversible), event stream.
7. **Schema validation.** Arguments are checked against the tool's JSON schema (required, types, enums, bounds, `additionalProperties: false`) before the call; the error text goes back as the result and nothing runs. The same check now guards built-in tools in `ToolRegistry::call`.
8. **Config.** A project's `./.judas/config.toml` cannot define MCP servers (opening a repo must not start programs); they are dropped with a message unless `JUDAS_PROJECT_MCP=1`.

Not covered: a server inside the sandbox can still read everything that is world-readable on the root filesystem (not the hidden directories), and a server with `network = true` can send what it reads anywhere. Give network to servers that need it, and only those.

## Installed servers (this machine, 2026-10-20)

Installed without sudo through `uv tool` from PyPI, official upstream sources:

| server | version | path | source |
|---|---|---|---|
| markitdown-mcp | 0.0.1a7 (markitdown 0.1.8, mcp SDK 2.3.0) | `~/.local/bin/markitdown-mcp` | github.com/microsoft/markitdown (Microsoft, MIT) |
| mcp-pdf | 2026.09.22 (FastMCP 4.1.2, PyMuPDF 2.0, pdfplumber 0.11.10) | `~/.local/bin/mcp-pdf` | github.com/rsp2k/mcp-pdf (MIT, single maintainer) |

Update with `uv tool upgrade markitdown-mcp mcp-pdf`. markitdown-mcp offers one tool, `convert_to_markdown(uri)` with `file:` URIs (`file:///home/you/doc.pdf`); under the sandbox only local files work (no network). mcp-pdf tried to check PyPI for a newer FastMCP at start (fails harmlessly with the network off) and prints a banner to stderr.
