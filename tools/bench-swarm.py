#!/usr/bin/env python3
"""Driver for tools/bench-swarm.sh: parallel chat completions against one Judas instance.

  bench-swarm.py URL --parallel N --max-tokens M [--serial]

Sends N identical requests (temperature 0, fixed prompt, max_tokens M) at once, or one after another with
--serial, and prints one JSON line: tokens (completion tokens as the server counts them, thinking included),
wall seconds, aggregate tok/s and mean per-request tok/s (each request over its own latency).
"""
import argparse, json, threading, time, urllib.request

PROMPT = ("Write a long, detailed explanation of how a four stroke engine works, covering intake, compression, "
          "power and exhaust, valve timing, and why each stage matters. Do not stop early.")


def one(url, max_tokens, out, i):
    body = json.dumps({"model": "swarm", "stream": False, "temperature": 0, "max_tokens": max_tokens,
                       "messages": [{"role": "user", "content": PROMPT}]}).encode()
    req = urllib.request.Request(url + "/v1/chat/completions", body, {"Content-Type": "application/json"})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=1800) as r:
            j = json.load(r)
        out[i] = (j.get("usage", {}).get("completion_tokens", 0), time.time() - t0, None)
    except Exception as e:  # noqa: BLE001
        out[i] = (0, time.time() - t0, str(e))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("url")
    ap.add_argument("--parallel", type=int, default=1)
    ap.add_argument("--max-tokens", type=int, default=256)
    ap.add_argument("--serial", action="store_true")
    a = ap.parse_args()
    out = [None] * a.parallel
    t0 = time.time()
    if a.serial:
        for i in range(a.parallel):
            one(a.url, a.max_tokens, out, i)
    else:
        ths = [threading.Thread(target=one, args=(a.url, a.max_tokens, out, i)) for i in range(a.parallel)]
        [t.start() for t in ths]
        [t.join() for t in ths]
    wall = time.time() - t0
    toks = sum(o[0] for o in out)
    per = [o[0] / o[1] for o in out if o[1] > 0 and o[0] > 0]
    errs = [o[2] for o in out if o[2]]
    print(json.dumps({"parallel": a.parallel, "serial": a.serial, "tokens": toks, "wall_s": round(wall, 2),
                      "agg_tps": round(toks / wall, 2) if wall else 0,
                      "per_req_tps": round(sum(per) / len(per), 2) if per else 0, "errors": errs[:2]}))


main()
