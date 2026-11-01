#!/usr/bin/env bash
# GPU test of the Strata backend: stop judas.service, load Qwen3.8-Flash-Next through `backend = "strata"` on a
# private port, run a short chat and a 400-token decode over /v1, print tok/s, then restore judas.service.
#
#   tools/test-strata.sh                run (builds build-strata with -DJUDAS_STRATA=ON if needed)
#
# The engine is judas itself (judas __strata-engine), so the build needs ROCm and the Strata sources (~/src/strata,
# read only) once; nothing of Strata is needed at run time except the model files.
#
# Env: STRATA_DIR (default ~/src/strata, build time only), STRATA_MODEL (first GGUF shard, default the IQ2_XS pack under
#      ~/models/strata), STRATA_PACK, STRATA_MTP, CTX (default 32768), PORT (default 18081), KEEP_STOPPED=1 to leave
#      judas.service stopped, JUDAS_FORCE=1 to skip the "VRAM is free" guard. Logs: $WORK/data/logs/strata.log.
# Needs the GPU to be free (stops judas.service for the duration; do not run while a game or a benchmark runs).
# Everything it writes goes to a temp dir; ~/.config/judas and the installed binary are not touched.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
STRATA_DIR="${STRATA_DIR:-$HOME/src/strata}"
MODEL="${STRATA_MODEL:-$HOME/models/strata/IQ2_XS/Qwen3.8-Flash-Next-GSQ-RCO-IQ2_XS-00001-of-00002.gguf}"
PACK="${STRATA_PACK:-$HOME/models/strata/packs/iq2_xs}"
MTP="${STRATA_MTP:-$HOME/models/strata/mtp/rt}"
CTX="${CTX:-32768}"
PORT="${PORT:-18081}"
WORK="$(mktemp -d /tmp/judas-strata-test.XXXXXX)"
say() { printf '\n[strata] %s\n' "$*"; }
fail() { printf '\n[strata] FAIL: %s\n' "$*" >&2; exit 1; }

for f in "$MODEL" "$PACK/dense.bin"; do
    [ -e "$f" ] || fail "missing $f"
done
[ -d "$MTP" ] || fail "missing MTP dir $MTP"

if [ ! -x build-strata/judas ]; then
    say "building build-strata (JUDAS_STRATA=ON)"
    cmake -S . -B build-strata -G Ninja -DCMAKE_BUILD_TYPE=Release -DJUDAS_STRATA=ON -DJUDAS_STRATA_DIR="$STRATA_DIR" \
        -DLLAMA_DIR="${LLAMA_DIR:-$HOME/src/llama.cpp}" -DLLAMA_LIB_DIR="${LLAMA_DIR:-$HOME/src/llama.cpp}/build/bin" >/dev/null
    cmake --build build-strata --target judas
fi

WAS_ACTIVE=0
if systemctl --user is-active --quiet judas.service; then WAS_ACTIVE=1; fi
JPID=""
cleanup() {
    [ -n "$JPID" ] && kill "$JPID" 2>/dev/null && wait "$JPID" 2>/dev/null || true
    if [ "$WAS_ACTIVE" = 1 ] && [ "${KEEP_STOPPED:-0}" != 1 ]; then
        say "restoring judas.service"
        systemctl --user start judas.service || true
    fi
    echo "[strata] work dir kept: $WORK (engine log: $WORK/data/logs/strata.log)"
}
trap cleanup EXIT

if [ "$WAS_ACTIVE" = 1 ]; then say "stopping judas.service"; systemctl --user stop judas.service; sleep 3; fi
if [ -z "${JUDAS_FORCE:-}" ]; then
    used=$(( $(cat /sys/class/drm/card1/device/mem_info_vram_used 2>/dev/null || echo 0) / 1048576 ))
    [ "$used" -lt 2048 ] || fail "$used MiB of VRAM still in use; stop the other GPU user (or JUDAS_FORCE=1)"
fi
# the known stuck-clock failure (Judas notes): check memory clock before loading
grep '\*' /sys/class/drm/card1/device/pp_dpm_mclk 2>/dev/null | sed 's/^/[strata] mclk: /' || true

cat > "$WORK/config.toml" <<EOF
[engine]
model = "titan"
max_tokens = 1024
[server]
port = $PORT
[models.titan]
backend = "strata"
path = "$MODEL"
strata_pack = "$PACK"
strata_mtp = "$MTP"
ctx_total = $CTX
[models.titan.sampling]
temp = 0.7
top_p = 0.95
top_k = 20
EOF

say "starting judas (loads ~35-55 GB into RAM; the PC may be slow for 1-3 minutes)"
JUDAS_CONFIG="$WORK/config.toml" JUDAS_DATA_DIR="$WORK/data" JUDAS_FORCE=1 \
    "$ROOT/build-strata/judas" serve > "$WORK/judas.log" 2>&1 &
JPID=$!
for i in $(seq 1 1300); do
    if curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then break; fi
    kill -0 "$JPID" 2>/dev/null || { tail -20 "$WORK/judas.log" >&2; fail "judas exited while loading"; }
    sleep 1
done
curl -fsS "http://127.0.0.1:$PORT/health" >/dev/null || fail "not ready after ~20 minutes (see $WORK/judas.log)"
say "ready after ${i}s"

python3 -I - "$PORT" <<'PY'
import json, sys, time, urllib.request
port = sys.argv[1]
def call(prompt, max_tokens):
    body = {"messages": [{"role": "user", "content": prompt}], "max_tokens": max_tokens, "stream": True,
            "stream_options": {"include_usage": True}, "chat_template_kwargs": {"enable_thinking": False}}
    req = urllib.request.Request(f"http://127.0.0.1:{port}/v1/chat/completions", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    t0 = time.time(); first = last = None; text = ""; usage = {}; chunks = 0; finish = None
    with urllib.request.urlopen(req, timeout=900) as r:
        for raw in r:
            line = raw.decode().strip()
            if not line.startswith("data: ") or line.endswith("[DONE]"):
                continue
            j = json.loads(line[6:])
            if j.get("usage"): usage = j["usage"]
            for c in j.get("choices", []):
                d = c.get("delta", {})
                piece = d.get("content") or ""
                if piece:
                    now = time.time(); first = first or now; last = now; chunks += 1; text += piece
                if c.get("finish_reason"): finish = c["finish_reason"]
    return text, usage, t0, first, last, chunks, finish
print("\n[strata] --- short chat ---")
text, usage, t0, first, last, chunks, finish = call("Reply with exactly: pong", 32)
print(repr(text), "finish:", finish, "first token after %.1fs" % ((first or t0) - t0))
print("\n[strata] --- 400-token decode ---")
text, usage, t0, first, last, chunks, finish = call(
    "Write a long, detailed essay about how mountains form. Do not stop early.", 400)
n = usage.get("completion_tokens") or chunks
span = (last - first) if first and last and last > first else None
print("completion tokens:", n, "finish:", finish, "time to first token: %.1fs" % ((first or t0) - t0))
if span:
    print("decode: %.1f tok/s (first to last chunk, %.1fs)" % ((n - 1) / span, span))
print("(Strata's own server measured ~47 tok/s with this model on this GPU)")
PY
say "engine log tail"
tail -4 "$WORK/data/logs/strata.log" || true
