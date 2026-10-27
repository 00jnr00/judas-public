#!/usr/bin/env python3
"""A tiny MCP server for Judas's tests. Newline-delimited JSON-RPC 2.0 on stdio.

  --era legacy   initialize handshake; server/discover answers "method not found" (a 2025-11-25 server)
  --era modern   no handshake; every request must carry _meta protocolVersion 2026-07-28
  --era both     answers either
  --pages N      tools/list returns N tools per page (with nextCursor)
  --silent-discover   never answers server/discover (a legacy server that ignores unknown methods)
Tools: echo, add, fail, slow (sleeps `seconds`), crash (exits), image, grow (adds a tool, sends list_changed), pid.
"""
import base64, json, os, sys, time

era = "legacy"
pages = 0
silent_discover = False
args = sys.argv[1:]
i = 0
while i < len(args):
    if args[i] == "--era": era = args[i + 1]; i += 1
    elif args[i] == "--pages": pages = int(args[i + 1]); i += 1
    elif args[i] == "--silent-discover": silent_discover = True
    i += 1

PNG = base64.b64encode(bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c4890000000d49444154789c6360000002000001e221bc330000000049454e44ae426082")).decode()

tools = [
    {"name": "echo", "description": "Echo text back.\n\nSecond paragraph that is dropped.",
     "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}, "n": {"type": "integer", "minimum": 1},
                                                      "mode": {"type": "string", "enum": ["a", "b"]}},
                     "required": ["text"], "additionalProperties": False},
     "annotations": {"readOnlyHint": True}},
    {"name": "add", "description": "Add two numbers.",
     "inputSchema": {"type": "object", "properties": {"a": {"type": "number"}, "b": {"type": "number"}}, "required": ["a", "b"]}},
    {"name": "fail", "description": "Always reports a tool error.", "inputSchema": {"type": "object"}},
    {"name": "slow", "description": "Sleep, then answer.",
     "inputSchema": {"type": "object", "properties": {"seconds": {"type": "number"}}}},
    {"name": "crash", "description": "Kill the server.", "inputSchema": {"type": "object"}},
    {"name": "image", "description": "Return a PNG.", "inputSchema": {"type": "object"}, "annotations": {"readOnlyHint": True}},
    {"name": "grow", "description": "Add a tool and announce it.", "inputSchema": {"type": "object"}},
    {"name": "pid", "description": "Server process id.", "inputSchema": {"type": "object"}, "annotations": {"readOnlyHint": True}},
    {"name": "run", "description": "Pretend to run a command.",
     "inputSchema": {"type": "object", "properties": {"command": {"type": "string"}, "path": {"type": "string"}}}},
]
initialized = False


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def reply(mid, result=None, error=None, modern=False):
    m = {"jsonrpc": "2.0", "id": mid}
    if error is not None: m["error"] = error
    else:
        if modern and isinstance(result, dict): result = dict(result, resultType="complete")
        m["result"] = result
    send(m)


def list_page(cursor):
    start = int(cursor) if cursor else 0
    n = pages or len(tools)
    chunk = tools[start:start + n]
    res = {"tools": chunk}
    if start + n < len(tools): res["nextCursor"] = str(start + n)
    return res


for line in sys.stdin:
    line = line.strip()
    if not line: continue
    msg = json.loads(line)
    method, mid, params = msg.get("method"), msg.get("id"), msg.get("params") or {}
    if method is None: continue
    meta = params.get("_meta", {})
    is_modern_req = meta.get("io.modelcontextprotocol/protocolVersion") == "2026-07-28"
    if mid is None:
        continue  # notifications (initialized, cancelled)
    if method == "server/discover":
        if silent_discover: continue
        if era == "legacy":
            reply(mid, error={"code": -32601, "message": "Method not found"}); continue
        reply(mid, {"supportedVersions": ["2026-07-28"] + (["2025-11-25"] if era == "both" else []),
                    "capabilities": {"tools": {"listChanged": True}}, "serverInfo": {"name": "fake", "version": "1"}}, modern=True)
        continue
    if method == "initialize":
        if era == "modern":
            reply(mid, error={"code": -32600, "message": "this server speaks 2026-07-28 only"}); continue
        initialized = True
        reply(mid, {"protocolVersion": "2025-11-25", "capabilities": {"tools": {"listChanged": True}},
                    "serverInfo": {"name": "fake", "version": "1"}})
        continue
    if era == "modern" or (era == "both" and is_modern_req):
        if not is_modern_req:
            reply(mid, error={"code": -32602, "message": "missing protocolVersion in _meta"}); continue
        modern = True
    else:
        if not initialized:
            reply(mid, error={"code": -32600, "message": "not initialized"}); continue
        modern = False
    if method == "tools/list":
        reply(mid, list_page(params.get("cursor")), modern=modern)
    elif method == "tools/call":
        name, a = params.get("name"), params.get("arguments") or {}
        if name == "echo":
            reply(mid, {"content": [{"type": "text", "text": "echo: " + a.get("text", "")}]}, modern=modern)
        elif name == "add":
            reply(mid, {"content": [{"type": "text", "text": str(a["a"] + a["b"])}]}, modern=modern)
        elif name == "fail":
            reply(mid, {"content": [{"type": "text", "text": "boom"}], "isError": True}, modern=modern)
        elif name == "slow":
            time.sleep(float(a.get("seconds", 5)))
            reply(mid, {"content": [{"type": "text", "text": "done"}]}, modern=modern)
        elif name == "crash":
            sys.stderr.write("fake server crashing on purpose\n"); sys.stderr.flush()
            os._exit(3)
        elif name == "image":
            reply(mid, {"content": [{"type": "text", "text": "here"}, {"type": "image", "data": PNG, "mimeType": "image/png"},
                                    {"type": "resource", "resource": {"uri": "file:///x.txt", "text": "inner"}}]}, modern=modern)
        elif name == "pid":
            reply(mid, {"content": [{"type": "text", "text": str(os.getpid())}]}, modern=modern)
        elif name == "run":
            reply(mid, {"content": [{"type": "text", "text": "ran"}]}, modern=modern)
        elif name == "grow":
            tools.append({"name": "extra", "description": "Appeared later.", "inputSchema": {"type": "object"}})
            reply(mid, {"content": [{"type": "text", "text": "grown"}]}, modern=modern)
            send({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})
        elif name == "extra":
            reply(mid, {"content": [{"type": "text", "text": "extra works"}]}, modern=modern)
        else:
            reply(mid, error={"code": -32602, "message": "Unknown tool: %s" % name})
    else:
        reply(mid, error={"code": -32601, "message": "Method not found: %s" % method})
