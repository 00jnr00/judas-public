# judas-mcp

A stdio MCP server (FastMCP, mcp 1.x) that lets any MCP client, such as an editor, a coding agent or another
assistant, hand work to the Judas daemon on 127.0.0.1:8079 (override with JUDAS_URL).

Tools: judas_up, judas_run, judas_status, judas_result, judas_wait, judas_message, judas_resume, judas_cancel,
judas_sessions, judas_state. judas_up only runs `systemctl --user start judas.service`; it never enables the unit.

Setup:

    cd tools/judas-mcp && uv venv .venv && uv pip install --python .venv/bin/python -e '.[test]'

Then register it with your client as a stdio server. Most clients take a JSON entry like this:

    {"mcpServers": {"judas": {"command": "/path/to/judas/tools/judas-mcp/.venv/bin/python",
                              "args": ["-m", "judas_mcp.server"]}}}

Tests: `.venv/bin/pytest` (mock HTTP server). Live smoke: `.venv/bin/python tests/smoke.py <dir under ~>`
(the daemon only allows writes under `permissions.allow_paths`, default `~`).
