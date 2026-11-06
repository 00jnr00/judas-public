"""Tiny fake OpenAI-compatible server for the offline web e2e tests.

POST /v1/chat/completions streams a canned reply chosen by a marker in the last user message
(see SCRIPTS). Tool calls are streamed as OpenAI tool_calls deltas. No model, no network.
"""
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# cards and swatch blocks use "key: value" lines, one "- " per item
CARDS = "```cards\n- title: Alpha\n  note: first\n  price: $5\n- title: Beta\n  note: second\n  image: http://example.invalid/pic.png\n```\n"
SWATCH = "```swatch\n- hex: #ff8800\n- hex: #0066cc\n- hex: #22aa55\n```\n"
TASKS = "### Steps\n- [x] done item\n- [ ] open item\n- [ ] another item\n"

TABLE = ("Say you take home **$3,000 a month**. Split it like this:\n\n| Bucket | % | Dollars/month | Pays for... |\n|---|:-:|--:|---|\n"
         "| Needs | 50% | $1,500 | Rent, groceries, utilities, car, insurance and a long tail of other fixed costs |\n"
         "| Wants | 30% | $900 | Dining out, `streaming`, hobbies |\n| Savings | 20% | $600 | Emergency fund |\n\nAfter the table.")

SCRIPTS = {
    "e2e-table": {"text": TABLE},
    "e2e-rich": {"text": "Here you go.\n\n" + CARDS + "\n" + SWATCH + "\n" + TASKS},
    "e2e-think": {"reasoning": "pondering the question", "text": "Answer after thinking.", "delay": 0.8},
    "e2e-learn": {"tool": ("learn_ask", {"question": "What is a pointer?", "kind": "free", "options": []})},
}
DEFAULT = {"text": "ok"}
# "e2e-att <id>" replies with an inline image of that uploaded attachment
ATT_RE = re.compile(r"e2e-att\s+([\w.%-]+)")


def pick(messages):
    last = next((m for m in reversed(messages) if m.get("role") == "user"), {})
    c = last.get("content", "")
    if isinstance(c, list):
        c = " ".join(p.get("text", "") for p in c if isinstance(p, dict))
    m = ATT_RE.search(c)
    if m:
        return {"text": "Here is the picture.\n\n![shot](/api/attachments/" + m.group(1) + ")\n"}
    if "e2e-hello" in c:
        return {"text": "Hello from the fake model."}
    for k, v in SCRIPTS.items():
        if k in c:
            return v
    return DEFAULT


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "fake-oai"
    requests = []  # bodies seen, for assertions

    def log_message(self, *a):
        pass

    def _json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path.rstrip("/").endswith("/models"):
            return self._json({"object": "list", "data": [{"id": "fake-model", "object": "model"}]})
        if self.path == "/health":
            return self._json({"status": "ok"})
        self._json({"error": "not found"}, 404)

    def _chunk(self, delta, finish=None):
        j = {"id": "fake", "object": "chat.completion.chunk", "model": "fake-model",
             "choices": [{"index": 0, "delta": delta, "finish_reason": finish}]}
        b = ("data: " + json.dumps(j) + "\n\n").encode()
        self.wfile.write(b"%x\r\n" % len(b) + b + b"\r\n")
        self.wfile.flush()

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(n) or b"{}")
        Handler.requests.append(body)
        if not self.path.endswith("/chat/completions"):
            return self._json({"error": "not found"}, 404)
        msgs = body.get("messages", [])
        # after a tool result the model just wraps up
        script = {"text": "Thanks."} if msgs and msgs[-1].get("role") == "tool" else pick(msgs)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        self._chunk({"role": "assistant"})
        if script.get("reasoning"):
            self._chunk({"reasoning_content": script["reasoning"]})
        time.sleep(script.get("delay", 0))
        if script.get("text"):
            t = script["text"]
            for i in range(0, len(t), 40):
                self._chunk({"content": t[i:i + 40]})
        if script.get("tool"):
            name, args = script["tool"]
            self._chunk({"tool_calls": [{"index": 0, "id": "call_1", "type": "function",
                                          "function": {"name": name, "arguments": json.dumps(args)}}]})
        self._chunk({}, "tool_calls" if script.get("tool") else "stop")
        done = b"data: [DONE]\n\n"
        self.wfile.write(b"%x\r\n" % len(done) + done + b"\r\n0\r\n\r\n")
        self.wfile.flush()


def start(port=0):
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


if __name__ == "__main__":
    s = start(int(__import__("os").environ.get("FAKE_PORT", "8081")))
    print("fake openai on", s.server_address)
    threading.Event().wait()
