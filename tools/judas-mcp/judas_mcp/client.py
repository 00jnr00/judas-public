"""Plain HTTP client and logic for the Judas daemon (no MCP dependency, easy to test)."""
import json
import os
import subprocess
import time
import urllib.error
import urllib.request

BASE = os.environ.get("JUDAS_URL", "http://127.0.0.1:8079").rstrip("/")
UNIT = os.environ.get("JUDAS_UNIT", "judas.service")
POLL_S = 5
TERMINAL = {"done", "paused", "error", "failed", "killed", "idle"}


class JudasError(Exception):
    pass


def clean(text):
    """gpt-oss sometimes leaks harmony control tokens into the final text; pull out the done summary."""
    if text and "<|" in text:
        import re
        m = re.search(r'to=functions\.done.*?\{.*?"summary"\s*:\s*"((?:[^"\\]|\\.)*)"', text, re.S)
        if m:
            try:
                return json.loads('"' + m.group(1) + '"')
            except ValueError:
                return m.group(1)
        text = re.sub(r"<\|[^|]*\|>", " ", text)
    return text


def trim(text, n=1500):
    text = text or ""
    if len(text) <= n:
        return text
    return text[:n] + f"\n[... truncated, {len(text) - n} more chars]"


def call(method, path, body=None, timeout=15):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            raw = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", "replace")
        try:
            msg = json.loads(raw).get("error", raw)
            msg = msg.get("message", msg) if isinstance(msg, dict) else msg
        except Exception:
            msg = raw
        raise JudasError(f"{method} {path} -> HTTP {e.code}: {trim(str(msg), 300)}")
    except (urllib.error.URLError, OSError) as e:
        raise JudasError(f"Judas daemon not reachable at {BASE} ({e}). Call judas_up first.")
    try:
        return json.loads(raw) if raw else {}
    except ValueError:
        return {"text": raw}


def is_up():
    try:
        call("GET", "/api/state", timeout=3)
        return True
    except JudasError:
        return False


def up(timeout_s=90):
    """Start the user unit if the daemon does not answer. Never enables it."""
    if is_up():
        st = call("GET", "/api/state")
        return {"started": False, "model": st.get("model"), "loaded": st.get("loaded")}
    r = subprocess.run(["systemctl", "--user", "start", UNIT], capture_output=True, text=True)
    if r.returncode != 0:
        raise JudasError(f"systemctl --user start {UNIT} failed: {trim(r.stderr, 300)}")
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        if is_up():
            st = call("GET", "/api/state")
            if st.get("loaded", True):
                return {"started": True, "model": st.get("model"), "loaded": st.get("loaded"),
                        "waited_s": round(time.time() - t0, 1)}
        time.sleep(2)
    raise JudasError(f"{UNIT} started but /api/state did not answer within {timeout_s} s")


def lead(agents):
    for a in agents:
        if not a.get("parent"):
            return a
    return agents[0] if agents else None


def _text(body):
    c = (body or {}).get("content")
    if isinstance(c, list):
        c = " ".join(p.get("text", "") for p in c if isinstance(p, dict))
    return c or ""


def last_assistant(messages, agent_id=None):
    for m in reversed(messages):
        if m.get("role") != "assistant":
            continue
        if agent_id and m.get("agent") != agent_id:
            continue
        t = _text(m.get("body")).strip()
        if t:
            return t
    return ""


def session_view(sid, limit=30):
    return call("GET", f"/api/sessions/{sid}?limit={limit}")


def engine_summary():
    try:
        s = call("GET", "/api/state", timeout=5)
    except JudasError as e:
        return {"error": str(e)}
    return {"model": s.get("model"), "ctx": f"{s.get('ctx_active')}/{s.get('ctx_total')}",
            "ctx_free": s.get("ctx_free"), "active": s.get("active"), "queued": s.get("queued"),
            "queue_len": len(s.get("queue") or []), "preemptions": s.get("preemptions"),
            "gen_tok_s": s.get("gen_tok_s")}


def status(sid):
    v = session_view(sid)
    turn = v.get("turn") or {}
    ags = v.get("agents") or []
    ld = lead(ags)
    return {
        "session": sid,
        "title": (v.get("session") or {}).get("title"),
        "turn_status": turn.get("status", "running" if turn.get("running") else "idle"),
        "running": bool(turn.get("running")),
        "reason": turn.get("reason") or "",
        "open_items": turn.get("open_items") or [],
        "agents": [{"id": a.get("id"), "role": a.get("role"), "status": a.get("status"),
                    "lead": not a.get("parent")} for a in ags],
        "lead_result": trim(clean((ld or {}).get("result")), 600),
        "last_assistant_text": trim(clean(last_assistant(v.get("messages") or [], (ld or {}).get("id"))), 800),
        "messages_total": v.get("total"),
        "engine": engine_summary(),
    }


def run(task, cwd, mode="code", wait_s=0):
    if mode not in ("chat", "cowork", "code"):
        raise JudasError("mode must be chat, cowork or code")
    u = up()
    s = call("POST", "/api/sessions", {"cwd": cwd, "mode": mode, "title": task[:60].replace("\n", " ")})
    sid = s["id"]
    call("POST", f"/api/sessions/{sid}/messages", {"content": task, "cwd": cwd})
    out = {"session": sid, "cwd": s.get("cwd"), "daemon_started": u["started"], "model": u["model"]}
    if wait_s and wait_s > 0:
        out["wait"] = wait(sid, wait_s)
        out["answer"] = result(sid).get("answer")
    return out


def wait(sid, timeout_s=600, poll_s=None):
    poll_s = POLL_S if poll_s is None else poll_s
    t0 = time.time()
    seen_running = False
    while True:
        st = status(sid)
        if st["running"] or st["turn_status"] == "running":
            seen_running = True
        elif st["turn_status"] in TERMINAL and (seen_running or st["turn_status"] != "idle"
                                                or time.time() - t0 > 4):
            st["waited_s"] = round(time.time() - t0, 1)
            return st
        if time.time() - t0 >= timeout_s:
            st["timed_out"] = True
            st["waited_s"] = round(time.time() - t0, 1)
            return st
        time.sleep(min(poll_s, max(0.0, timeout_s - (time.time() - t0))) or 0.1)


def files_changed(sid):
    try:
        r = call("GET", f"/api/snapshots?session={sid}&limit=200")
    except JudasError as e:
        return None, str(e)
    seen = {}
    for sn in r.get("snapshots", []):
        if sn.get("reverted"):
            continue
        for f in sn.get("files", []):
            seen[f.get("path")] = "created" if not f.get("before_hash") else (
                "deleted" if not f.get("after_hash") else "modified")
    return seen, None


def result(sid):
    v = session_view(sid, 40)
    ld = lead(v.get("agents") or [])
    turn = v.get("turn") or {}
    answer = (ld or {}).get("result") or last_assistant(v.get("messages") or [], (ld or {}).get("id")) \
        or last_assistant(v.get("messages") or [])
    files, err = files_changed(sid)
    out = {"session": sid, "turn_status": turn.get("status"), "reason": turn.get("reason") or "",
           "lead_status": (ld or {}).get("status"), "answer": trim(clean(answer), 4000)}
    if files is None:
        out["files_changed"] = f"unavailable ({err})"
    else:
        out["files_changed"] = [{"path": p, "change": c} for p, c in files.items()][:100]
    return out


def message(sid, text):
    call("POST", f"/api/sessions/{sid}/messages", {"content": text})
    return {"session": sid, "sent": True}


def resume(sid):
    return call("POST", f"/api/sessions/{sid}/resume", {})


def cancel(sid):
    return call("POST", f"/api/sessions/{sid}/cancel", {})


def sessions(limit=10):
    r = call("GET", f"/api/sessions?limit={int(limit)}")
    return {"sessions": [{"id": s["id"], "title": trim(s.get("title"), 70), "mode": s.get("mode"),
                          "cwd": s.get("cwd"), "turn_status": s.get("turn_status"),
                          "running": s.get("running"), "open_items": s.get("open_items"),
                          "updated_ms": s.get("updated_ms")} for s in r.get("sessions", [])],
            "more": r.get("more")}
