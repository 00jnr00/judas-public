import json
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

from judas_mcp import client as c

STATE = {"turn": {"running": True, "status": "running", "reason": "", "open_items": ["a"]}, "polls": 0}
CALLS = []


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        CALLS.append(("GET", self.path))
        if self.path == "/api/state":
            return self._send({"model": "m", "loaded": True, "ctx_active": 5, "ctx_total": 100, "ctx_free": 90,
                               "active": 1, "queued": 0, "queue": [], "preemptions": 0, "gen_tok_s": 50})
        if self.path.startswith("/api/snapshots"):
            return self._send({"snapshots": [{"reverted": False, "files": [
                {"path": "/x/hello.txt", "before_hash": "", "after_hash": "h"}]}]})
        if self.path.startswith("/api/sessions?"):
            return self._send({"sessions": [{"id": "s1", "title": "t", "mode": "code", "turn_status": "done"}], "more": False})
        if self.path.startswith("/api/sessions/s1"):
            STATE["polls"] += 1
            if STATE["polls"] >= 3:
                STATE["turn"] = {"running": False, "status": "done", "reason": "", "open_items": []}
            return self._send({"session": {"title": "t"}, "total": 2, "turn": STATE["turn"],
                               "agents": [{"id": "a1", "parent": "", "role": "lead", "status": "done", "result": "all done"}],
                               "messages": [{"id": 1, "agent": "a1", "role": "assistant", "body": {"content": "hello there"}}]})
        self._send({"error": {"message": "no such session"}}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(n) or b"{}")
        CALLS.append(("POST", self.path, body))
        if self.path == "/api/sessions":
            return self._send({"id": "s1", "cwd": body.get("cwd")})
        if self.path.endswith("/resume"):
            return self._send({"error": "no paused turn"}, 409)
        self._send({"ok": True, "session": "s1"})


@pytest.fixture(autouse=True)
def server(monkeypatch):
    srv = HTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    monkeypatch.setattr(c, "BASE", f"http://127.0.0.1:{srv.server_port}")
    monkeypatch.setattr(c, "POLL_S", 0.05)
    STATE.update(polls=0, turn={"running": True, "status": "running", "reason": "", "open_items": ["a"]})
    CALLS.clear()
    yield
    srv.shutdown()


def test_up_already_running():
    assert c.up() == {"started": False, "model": "m", "loaded": True}


def test_status_fields():
    st = c.status("s1")
    assert st["turn_status"] == "running" and st["open_items"] == ["a"]
    assert st["agents"][0]["lead"] and st["last_assistant_text"] == "hello there"
    assert st["engine"]["ctx"] == "5/100"


def test_wait_until_done():
    st = c.wait("s1", 30, poll_s=0.05)
    assert st["turn_status"] == "done" and "timed_out" not in st


def test_wait_timeout():
    STATE["polls"] = -10000
    st = c.wait("s1", 0.2, poll_s=0.05)
    assert st.get("timed_out")


def test_result_files():
    STATE["turn"] = {"running": False, "status": "done"}
    r = c.result("s1")
    assert r["answer"] == "all done"
    assert r["files_changed"] == [{"path": "/x/hello.txt", "change": "created"}]


def test_run_with_wait():
    out = c.run("do it", "/tmp", wait_s=30)
    assert out["session"] == "s1" and out["answer"] == "all done"
    posts = [x for x in CALLS if x[0] == "POST"]
    assert posts[0][2]["mode"] == "code" and posts[1][2]["content"] == "do it"


def test_errors_explicit():
    with pytest.raises(c.JudasError, match="404"):
        c.status("nope")
    with pytest.raises(c.JudasError, match="409"):
        c.resume("s1")
    with pytest.raises(c.JudasError):
        c.run("x", "/tmp", mode="bogus")


def test_sessions_and_message():
    assert c.sessions(5)["sessions"][0]["id"] == "s1"
    assert c.message("s1", "hi")["sent"]


def test_trim():
    assert "truncated" in c.trim("x" * 5000, 100)


def test_clean_harmony_leak():
    raw = '<|channel|>analysis<|message|>x<|end|><|start|>assistant<|channel|>final to=functions.done<|channel|>commentary<|constrain|>json<|message|>{"summary":"Created it"}'
    assert c.clean(raw) == "Created it"
    assert c.clean("plain") == "plain"
