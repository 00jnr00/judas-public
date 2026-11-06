# Security review

Scope: the HTTP server (`src/server/`), the forwarder for the external tutor app, attachments, the web app's
rendering of model output, the file tools and the shell sandbox. Threat model: one person's own machine, no
login on the API, so the browser is the attacker's route in (a hostile page, DNS rebinding, a prompt-injected
model) and the model is the other.

Method: read the code paths, then regression tests for each fix. Line numbers refer to the tree after this change.

Limit of this pass: the sandbox this review ran in had no copy of the model runtime library the project links
against, so the full build and `ctest` were not run here. Every changed file and test was syntax checked, and the
shell-floor patterns were run in a standalone harness against both the new refusal list and a list of ordinary
commands (no misses, no false positives). Run `ctest` before merging.

## Findings

| # | Severity | Where | Class | Status |
|---|----------|-------|-------|--------|
| 1 | High | `src/server/visual_proxy.cpp:198` | CSRF through the forwarder: writes with a foreign `Origin` were relayed to the tutor with that header intact; no cross-site check at all | Fixed |
| 2 | Medium | `src/tools/builtin_tools.cpp:125` (`read_denied`) | `permissions.deny_paths` ("always refused") was only enforced for writes; read_file, list_dir, glob and grep ignored it | Fixed |
| 3 | Medium | `src/tools/builtin_tools.cpp:694`, `src/mcp/launch.cpp:167` | Sandbox did not hide `deny_paths`, and hid only directories, so a secret or denied single file stayed readable from the shell | Fixed |
| 4 | Medium | `src/tools/builtin_tools.cpp:575`, `:605` (`blocked_command`) | Shell floor bypasses: `sh -c 'sudo ..'`, `eval`, `sudo${IFS}id`, `s""udo`, `$'\x73udo'`, `/usr/bin/sud?`, `{sudo,id}`, `sudo<x`; unlisted privileged tools; `sed -n -i` / `--in-place`, `perl -pi`, `>|` against `/etc/passwd`; glob spellings of `/etc/shadow` | Fixed (floor only grew) |
| 5 | Medium | `src/server/server.cpp:530` (`frame_csp`) | Clickjacking: the app could be framed by any site that knew the local address, and a click on Approve is a write; no `script-src`, `object-src`, `base-uri`, `form-action` | Fixed |
| 6 | Medium | `src/server/server.cpp:888` | A browser-marked cross-site write with no `Origin` header skipped the CSRF check | Fixed |
| 7 | Low | `src/tools/builtin_tools.cpp:105` | glob, grep (fallback) and list_dir followed or listed entries under the tree that are symlinks to denied files, and listed names inside secret dirs | Fixed (`hidden_from_listing`) |
| 8 | Low | `src/server/visual_proxy.cpp:198` | Forwarder accepted an absolute-form request target (`GET http://other/ ..`); it still only reached the configured origin, but the line was relayed verbatim | Fixed (origin-form only, else 400) |
| 9 | Low | `src/server/server.cpp:1157` | Attachment responses had no CSP; types are already limited to a safe list plus `nosniff` | Fixed (`default-src 'none'; sandbox` except PDF) |
| 10 | Low | `src/server/server.cpp:909` | No `X-Frame-Options`, `Referrer-Policy`, `nosniff` on most responses | Fixed (post-routing handler) |
| 11 | Low | `src/server/server.cpp:882` | Upload bodies are buffered whole (up to 64 MB per request, 32 workers) before `attachments.max_bytes` is checked, and there is no total attachment quota | Open |
| 12 | Low | `src/server/server.cpp:886` | An empty `Host` header passes the host check (HTTP/1.0 clients). DNS rebinding always sends a name, so it is not exploitable from a browser | Open |
| 13 | Low | forwarder | Cookies set by the tutor are shared with the main app (cookies ignore ports). Judas sets no cookies, so only the tutor's own cookie could shadow another | Open |
| 14 | Low | `src/tools/builtin_tools.cpp` shell | `bwrap` is run without `--new-session`; no terminal is attached to the shell so terminal-injection does not apply, but adding it needs the timeout kill reworked | Open |
| 15 | Low | `src/tools/builtin_tools.cpp:60` | `secret_paths` entries are compared unresolved against resolved paths; if `$HOME` is itself a symlink, a literal `~/.something` entry can miss (`.ssh` and `.gnupg` are also matched by name) | Open |
| 16 | Low | shell floor | The string floor is a courtesy by design: an interpreter can build a command name at run time (`python -c`). The real floor is `no_new_privs` (set by bwrap and by `setpriv` in non-sandbox mode) plus the read-only root | Documented |

## What was checked and found sound

- **Host allowlist (DNS rebinding):** every request on the main port and the forwarder port goes through
  `host_allowed` before routing. IPv6 literals and ports parse correctly.
- **CORS:** headers are only sent to origins in `server.cors_origins`; a listed origin is echoed, never `*`
  unless the config says so. Preflight answers carry no secrets.
- **CSRF on the main port:** non-GET requests with an `Origin` that is neither same-origin nor listed get 403,
  including `Origin: null` (sandboxed frames, `data:` pages).
- **Path traversal in attachments:** ids must be 16 lowercase hex characters; the stored file name is rebuilt from
  the id and any `/` or `..` in metadata is rejected; names shown to the model are sanitized. Skins are looked up
  by name in a loaded list; mach12 sketch ids and dates are validated.
- **Served types:** only png, jpeg, gif, webp, bmp and pdf are served inline; everything else (html, svg, xml)
  comes back as `text/plain` with `nosniff`.
- **Upload limit:** `attachments.max_bytes` is enforced (413) in both raw and multipart uploads.
- **Forwarder as open proxy:** the upstream client is built from the configured origin only; `Host` is not
  forwarded; a request cannot name another host. Hop-by-hop headers are dropped, `Origin`/`Referer` pointing at
  the forwarder are mapped to the tutor's own origin.
- **Web rendering:** `esc()` runs on all text before the small markdown pass; links are limited to `http(s)` (and
  `mailto:` on the phone page) and always `noopener noreferrer`; images are limited to `http(s)`, `/api/attachments/`
  and four raster `data:` types, and remote images load only after a click; card and swatch fields are escaped or
  validated (`#hex` only in the style attribute); attachment chips escape every attribute; KaTeX is 0.16.11 with
  `trust` left off. There is no inline script and no `eval`/`new Function` in `web/`.
- **File tools:** `secret_paths`, `.ssh`/`.gnupg` path components, secret file names (`.env`, `*.pem`, `id_*`),
  and Judas's own config, data and `secrets.env` are checked on both the given and the resolved spelling.

## What changed

1. **Forwarder** (`visual_proxy.cpp`): pre-routing handler now also refuses a write whose `Origin` is not the
   forwarder's own origin, a cross-site write without `Origin`, and any request target that is not an absolute
   path. Test: `tests/test_server.cpp` (foreign origin, `Origin: null`, `Sec-Fetch-Site: cross-site`).
2. **Main server** (`server.cpp`): cross-site writes without `Origin` are refused; the page CSP now also sets
   `script-src 'self'`, `object-src 'none'`, `base-uri 'self'`, `form-action 'self'`, `frame-ancestors 'self'`;
   `X-Frame-Options`, `X-Content-Type-Options` and `Referrer-Policy` on every response; attachments get a locked
   CSP. Test: `tests/test_server.cpp`.
3. **deny_paths** (`builtin_tools.cpp`, `launch.cpp`): enforced on reads (both spellings), filtered out of
   list_dir, glob and grep including symlinks that resolve to a denied file, and hidden inside the bwrap shell and
   MCP sandboxes (directories by tmpfs, files by binding `/dev/null`). Tests: `tests/test_tools.cpp`,
   `tests/test_sandbox.cpp` (the bwrap case runs only where user namespaces work).
4. **Shell floor** (`builtin_tools.cpp`): the check now runs on the raw command and on a folded form (quotes and
   backslashes removed, `${IFS}` as a space, `\xNN` and octal escapes decoded). Added launchers (`sh`, `bash`,
   `eval`, `busybox`, `builtin`, `strace`, `watch`, `ionice`), a wider terminator set (`<`, `>`, `$`, `,`, `}`), a
   glob check on command words, more names (`sudo-rs`, `sudoedit`, `gpasswd`, `useradd`, `userdel`, `groupadd`,
   `groupmod`, `groupdel`, `adduser`, `deluser`, `vipw`, `vigr`, `machinectl`), more auth files, and more write
   verbs. Nothing was removed or loosened: every old pattern is a subset of the new one. Test:
   `tests/test_shellblock.cpp` (new refusals, plus ordinary commands that must still run).
