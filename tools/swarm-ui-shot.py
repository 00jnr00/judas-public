#!/usr/bin/env python3
"""Screenshot harness for the Swarm teams UI (stdlib only).

Serves web/ statically plus canned API responses from tools/swarm-ui-fixture.json (the session, /team, skins,
swarm state and an SSE replay), then drives headless Chromium against http://127.0.0.1:<port>/#swarm and writes
PNGs. No daemon, no model and no network beyond localhost are involved.

  python3 tools/swarm-ui-shot.py [--out DIR] [--skin NAME ...] [--sizes 1440x1000,390x844]
                                 [--tabs map,panes,table,file] [--budget MS] [--hot-budget MS]
                                 [--serve] [--check-overflow]

Every tab is opened with ?tab=NAME (the page also remembers the last tab in localStorage; the query wins). The Map
tab gets one more shot with a short virtual-time budget (--hot-budget) so a hot-potato packet is mid-flight.

--serve only runs the fake daemon (open the printed URL in a browser). --check-overflow also runs a DOM probe
in a second headless run and prints whether the page scrolls horizontally.
"""
import argparse
import glob
import json
import mimetypes
import os
import shutil
import subprocess
import sys
import threading
import time
import tomllib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(os.path.dirname(HERE), "web")
FIXTURE = json.load(open(os.path.join(HERE, "swarm-ui-fixture.json")))
SID = FIXTURE["session"]["session"]["id"]
DEFAULT_SKIN = ["judas"]
HOLD = [False]   # keep SSE open (only for --serve)
PROBE = ""   # set when --check-overflow is used


def load_repo_skins():
    """Add every skins/*.toml to the fixture's theme list (colors fall back to the fixture's judas, like the C++ loader)."""
    base = next(x for x in FIXTURE["skins"]["skins"] if x["name"] == "judas")
    toks = {"radius": "8px", "radius_sm": "4px", "space": "8px"}
    for f in sorted(glob.glob(os.path.join(os.path.dirname(HERE), "skins", "*.toml"))):
        t = tomllib.load(open(f, "rb"))
        name = t.get("name", os.path.basename(f)[:-5])
        entry = {"name": name, "label": t.get("label", name), "dark": t.get("dark", True),
                 "colors": {**base["colors"], **t.get("colors", {})}, "tokens": {**toks, **t.get("tokens", {})}}
        FIXTURE["skins"]["skins"] = [x for x in FIXTURE["skins"]["skins"] if x["name"] != name] + [entry]


def skin_css(name):
    skins = {s["name"]: s for s in FIXTURE["skins"]["skins"]}
    s = skins.get(name) or skins[DEFAULT_SKIN[0]]
    colors = dict(s["colors"])
    acc = colors.get("accent", "")
    if len(acc) == 7 and acc.startswith("#"):   # derived like the server's skin_vars
        colors["accent-soft"] = "rgba(%d,%d,%d,0.16)" % (int(acc[1:3], 16), int(acc[3:5], 16), int(acc[5:7], 16))
    body = ";".join("--%s:%s" % (k, v) for k, v in colors.items())
    body += "".join(";--%s:%s" % (k.replace("_", "-"), v) for k, v in s.get("tokens", {}).items())
    return ":root{%s;color-scheme:%s}" % (body, "dark" if s["dark"] else "light")


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def js(self, obj, code=200):
        self.send(code, json.dumps(obj), "application/json")

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(n)
        self.js({"ok": True})

    do_PATCH = do_POST
    do_DELETE = do_POST

    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        q = parse_qs(u.query)
        if p == "/skin.css":
            return self.send(200, skin_css((q.get("name") or [DEFAULT_SKIN[0]])[0]), "text/css")
        if p == "/state":
            return self.js(FIXTURE["state"])
        if p == "/api/skins":
            return self.js(FIXTURE["skins"])
        if p == "/api/swarm/state":
            return self.js(FIXTURE["swarm_state"])
        if p == "/api/sessions":
            return self.js({"sessions": FIXTURE["sessions"], "more": False})
        if p == "/api/sessions/%s" % SID:
            return self.js(FIXTURE["session"])
        if p == "/api/sessions/%s/team" % SID:
            return self.js(FIXTURE["team"])
        if p == "/api/sessions/%s/events" % SID:
            return self.sse(int((q.get("since") or [self.headers.get("Last-Event-ID") or "0"])[0] or 0), True)
        if p == "/api/events":
            return self.sse(0, False)
        if p.startswith("/api/") or p in ("/health", "/v1/models"):
            return self.js({})
        if p == "/probe.js":
            return self.send(200, PROBE, "application/javascript")
        # static
        rel = "index.html" if p in ("/", "") else p.lstrip("/")
        f = os.path.normpath(os.path.join(WEB, rel))
        if not f.startswith(WEB) or not os.path.isfile(f):
            return self.send(404, "not found", "text/plain")
        ctype = mimetypes.guess_type(f)[0] or "application/octet-stream"
        data = open(f, "rb").read()
        if rel == "index.html" and "probe=1" in u.query:
            data = data.replace(b"</body>", b'<script src="/probe.js"></script></body>')
        self.send(200, data, ctype)

    def sse(self, since, replay):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            if replay:
                for i, ev in enumerate(FIXTURE["events"], 1):
                    if i <= since:
                        continue
                    ev = dict(ev)
                    delay = ev.pop("_delay_ms", 0)   # real-time pause so the map exists when the hand-offs arrive
                    if delay:
                        time.sleep(delay / 1000.0)
                    self.wfile.write(("id: %d\ndata: %s\n\n" % (i, json.dumps(ev))).encode())
                    self.wfile.flush()
            if HOLD[0]:
                while True:
                    self.wfile.write(b": ping\n\n")
                    self.wfile.flush()
                    time.sleep(1)
            self.close_connection = True   # headless virtual time never settles on an open stream
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


def main():
    global PROBE
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/judas-swarm-ui-shots")
    ap.add_argument("--skin", action="append", help="skin name from the fixture (default: judas)")
    ap.add_argument("--sizes", default="1440x1000,390x844,390x1800")
    ap.add_argument("--tabs", default="map,panes,table,file")
    ap.add_argument("--budget", type=int, default=6000, help="virtual time budget in ms")
    ap.add_argument("--hot-budget", type=int, default=1000, help="short budget for the mid-animation Map shot")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--serve", action="store_true")
    ap.add_argument("--check-overflow", action="store_true")
    ap.add_argument("--repo-skins", action="store_true", help="also serve every skins/*.toml as a theme")
    ap.add_argument("--hash", default="swarm", help="location hash to open")
    a = ap.parse_args()
    if a.repo_skins:
        load_repo_skins()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), H)
    srv.daemon_threads = True
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d/" % port
    if a.serve:
        HOLD[0] = True
        print(base + "#swarm", flush=True)
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            return
    chrome = shutil.which("chromium") or shutil.which("chromium-browser") or shutil.which("google-chrome")
    if not chrome:
        sys.exit("no chromium found")
    os.makedirs(a.out, exist_ok=True)
    udir = os.path.join(a.out, ".profile")
    def shoot(skin, tab, size, budget, tag):
        w, h = size.split("x")
        png = os.path.join(a.out, "swarm-%s-%s%s-%s.png" % (skin, tab, tag, size))
        url = base + "?tab=" + tab + "#" + a.hash
        cmd = [chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
               "--user-data-dir=" + udir, "--window-size=%s,%s" % (w, h),
               "--virtual-time-budget=%d" % budget, "--screenshot=" + png, url]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        print(("ok  " if os.path.isfile(png) else "FAIL"), png)
        if not os.path.isfile(png):
            print(r.stderr[-800:])
        return url

    for skin in (a.skin or ["judas"]):
        DEFAULT_SKIN[0] = skin
        for size in a.sizes.split(","):
            w, h = size.split("x")
            for tab in a.tabs.split(","):
                shoot(skin, tab, size, a.budget, "")
                if tab == "map":
                    shoot(skin, tab, size, a.hot_budget, "-hot")
                if a.check_overflow:
                    PROBE = ("(function(){setTimeout(function(){var d=document.documentElement;"
                             "var o=document.createElement('pre');o.id='probe-out';"
                             "var wk=document.querySelector('.work');"
                             "o.textContent=JSON.stringify({scrollW:d.scrollWidth,clientW:d.clientWidth,bodyScrollW:document.body.scrollWidth,"
                             "workScrollW:wk?wk.scrollWidth:0,workClientW:wk?wk.clientWidth:0,"
                             "panes:document.querySelectorAll('.tpane').length,cards:document.querySelectorAll('.tmc').length,"
                             "rows:document.querySelectorAll('.tm-table tbody tr').length,"
                             "mapScrollL:(document.getElementById('team-map')||{}).scrollLeft,"
                             "mapScrollW:(document.getElementById('team-map')||{}).scrollWidth,"
                             "mapClientW:(document.getElementById('team-map')||{}).clientWidth});"
                             "document.body.appendChild(o);},%d);})();" % max(1000, a.budget - 1500))
                    dump = [chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--user-data-dir=" + udir,
                            "--window-size=%s,%s" % (w, h), "--virtual-time-budget=%d" % a.budget, "--dump-dom",
                            base + "?probe=1&tab=" + tab + "#" + a.hash]
                    out = subprocess.run(dump, capture_output=True, text=True, timeout=120).stdout
                    i = out.find('id="probe-out"')
                    print("    overflow probe (%s %s):" % (tab, size), out[i:i + 400].split(">", 1)[-1].split("<", 1)[0] if i >= 0 else "no probe output")
    srv.shutdown()


if __name__ == "__main__":
    main()
