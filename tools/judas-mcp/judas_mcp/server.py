"""FastMCP stdio server: drive the Judas daemon from any MCP client."""
import json

from mcp.server.fastmcp import FastMCP

from . import client as c

mcp = FastMCP("judas")


def _wrap(fn, *a, **k):
    try:
        return json.dumps(fn(*a, **k), indent=1, default=str)
    except c.JudasError as e:
        return json.dumps({"error": str(e)})
    except Exception as e:  # keep the server alive, report explicitly
        return json.dumps({"error": f"{type(e).__name__}: {e}"})


@mcp.tool()
def judas_up() -> str:
    """Ensure the Judas daemon is running (starts judas.service if :8079 is silent; never enables it)."""
    return _wrap(c.up)


@mcp.tool()
def judas_run(task: str, cwd: str, mode: str = "code", wait_s: int = 0) -> str:
    """Start a Judas session on a task in cwd. Returns the session id. wait_s>0 waits that long for the turn to finish and returns status plus the final answer."""
    return _wrap(c.run, task, cwd, mode, wait_s)


@mcp.tool()
def judas_status(session: str) -> str:
    """Turn status, reason, open plan items, agents, last assistant text and engine KV/queue summary."""
    return _wrap(c.status, session)


@mcp.tool()
def judas_result(session: str) -> str:
    """Final answer of the lead agent plus files changed (from the snapshot journal)."""
    return _wrap(c.result, session)


@mcp.tool()
def judas_wait(session: str, timeout_s: int = 600) -> str:
    """Block until the turn is done, paused, failed or killed (or timeout); returns judas_status."""
    return _wrap(c.wait, session, timeout_s)


@mcp.tool()
def judas_message(session: str, text: str) -> str:
    """Send a follow-up message to a session (starts a new turn)."""
    return _wrap(c.message, session, text)


@mcp.tool()
def judas_resume(session: str) -> str:
    """Resume a paused turn (409 error if there is none)."""
    return _wrap(c.resume, session)


@mcp.tool()
def judas_cancel(session: str) -> str:
    """Cancel the session's running turn and agents."""
    return _wrap(c.cancel, session)


@mcp.tool()
def judas_sessions(limit: int = 10) -> str:
    """List recent sessions, newest first."""
    return _wrap(c.sessions, limit)


@mcp.tool()
def judas_state() -> str:
    """Raw engine state: model, context use, queue, preemptions, tokens per second."""
    return _wrap(lambda: c.call("GET", "/api/state"))


def main():
    mcp.run()


if __name__ == "__main__":
    main()
