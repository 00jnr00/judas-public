#!/usr/bin/env bash
# ROCm/HIP test: build llama.cpp with GGML_HIP for this GPU, build Judas against it, run the unit tests, and
# (with a model) load it on a private port and measure single-stream and 4-stream decode over /v1.
#
#   tools/test-rocm.sh                      build + unit tests only
#   tools/test-rocm.sh --model x.gguf       also load the model and run the smoke + throughput test
#
# Env: LLAMA_DIR (default ~/src/llama.cpp), GPU_TARGET (default: first gfx from rocminfo), JOBS, JUDAS_TEST_TMP,
#      PORT (default 18079), JUDAS_FORCE=1 to load even when VRAM is already in use.
# It never touches your running daemon, its config or its data: everything goes to a temp dir.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
LLAMA_DIR="${LLAMA_DIR:-$HOME/src/llama.cpp}"
JOBS="${JOBS:-$(nproc)}"
PORT="${PORT:-18079}"
MODEL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --model) MODEL="$2"; shift 2 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

say() { printf '\n[rocm] %s\n' "$*"; }
fail() { printf '\n[rocm] FAIL: %s\n' "$*" >&2; exit 1; }

command -v rocminfo >/dev/null || fail "rocminfo not found; install ROCm (hip-runtime-amd, rocblas, rocminfo)"
command -v hipconfig >/dev/null || [ -x /opt/rocm/bin/hipconfig ] || fail "hipconfig not found; install the HIP SDK"
GPU_TARGET="${GPU_TARGET:-$(rocminfo | grep -m1 -o 'gfx[0-9a-f]\+' || true)}"
[ -n "$GPU_TARGET" ] || fail "no gfx target from rocminfo; set GPU_TARGET (e.g. gfx1030)"
[ -d "$LLAMA_DIR/ggml" ] || fail "llama.cpp source not found at $LLAMA_DIR (set LLAMA_DIR)"
say "GPU target $GPU_TARGET, llama.cpp $(git -C "$LLAMA_DIR" rev-parse --short HEAD 2>/dev/null || echo '?')"

HIPB="$LLAMA_DIR/build-judas-hip"
say "building llama.cpp with HIP -> $HIPB"
HIPCXX="$(hipconfig -l 2>/dev/null || echo /opt/rocm/llvm/bin)/clang" HIP_PATH="$(hipconfig -R 2>/dev/null || echo /opt/rocm)" \
cmake -S "$LLAMA_DIR" -B "$HIPB" -G Ninja -DCMAKE_BUILD_TYPE=Release -DGGML_HIP=ON -DGPU_TARGETS="$GPU_TARGET" \
    -DGGML_VULKAN=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF >/dev/null
cmake --build "$HIPB" -j "$JOBS" --target llama llama-common
[ -n "$(ls "$HIPB"/bin/libggml-hip.so* 2>/dev/null)" ] || fail "libggml-hip.so was not built"

JB="$ROOT/build-rocm"
say "building Judas against the HIP llama.cpp -> $JB"
cmake -S "$ROOT" -B "$JB" -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLAMA_DIR="$LLAMA_DIR" -DLLAMA_LIB_DIR="$HIPB/bin" -DJUDAS_TEST_TMP="${JUDAS_TEST_TMP:-/tmp/judas-tests}" >/dev/null
cmake --build "$JB" -j "$JOBS"
ldd "$JB/judas" | grep -q "$HIPB/bin/libllama" || fail "judas is not linked to the HIP libllama (check LLAMA_LIB_DIR)"

say "unit tests (network tests off)"
JUDAS_TESTS_NO_NET=1 ctest --test-dir "$JB" -j "$JOBS" --output-on-failure

if [ -z "$MODEL" ]; then
    say "PASS: build and unit tests. Add --model PATH.gguf for the GPU smoke test."
    exit 0
fi
[ -f "$MODEL" ] || fail "model not found: $MODEL"

TMP="$(mktemp -d)"
LOG="$TMP/serve.log"
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null && wait "$PID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT
cat > "$TMP/config.toml" <<EOF
[engine]
model = "rocm-test"
max_agents = 4

[models.rocm-test]
path = "$MODEL"
gpu_layers = 99
ctx_total = 16384
ctx_per_agent = 4096
flash_attn = true
EOF
export JUDAS_CONFIG="$TMP/config.toml" JUDAS_DATA_DIR="$TMP/data" JUDAS_PORT="$PORT"
"$JB/judas" config validate >/dev/null || fail "generated config did not validate"

say "loading $(basename "$MODEL") on port $PORT"
"$JB/judas" serve >"$LOG" 2>&1 &
PID=$!
for _ in $(seq 1 300); do
    curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break
    kill -0 "$PID" 2>/dev/null || { tail -30 "$LOG" >&2; fail "judas serve exited while loading"; }
    sleep 1
done
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null || { tail -30 "$LOG" >&2; fail "no /health after 300 s"; }
grep -qi "rocm\|hip\|ROCm" "$LOG" || echo "[rocm] note: the load log does not mention ROCm/HIP; check $LOG below" >&2

python3 - "$PORT" <<'PY' || { tail -40 "$LOG" >&2; fail "smoke test failed"; }
import json, sys, time, threading, urllib.request
port = sys.argv[1]
url = f"http://127.0.0.1:{port}/v1/chat/completions"
def one(prompt, n, stream=False, out=None):
    body = {"model": "rocm-test", "max_tokens": n, "temperature": 0,
            "messages": [{"role": "user", "content": prompt}], "stream": stream}
    req = urllib.request.Request(url, json.dumps(body).encode(), {"Content-Type": "application/json"})
    t0 = time.time(); first = None; chunks = 0; toks = 0; text = ""
    with urllib.request.urlopen(req, timeout=600) as r:
        if not stream:
            j = json.load(r); toks = j["usage"]["completion_tokens"]; text = j["choices"][0]["message"].get("content") or ""
        else:
            for line in r:
                line = line.strip()
                if not line.startswith(b"data:") or line == b"data: [DONE]": continue
                d = json.loads(line[5:])
                if d.get("choices") and (d["choices"][0].get("delta") or {}):
                    chunks += 1; first = first or time.time()
                if d.get("usage"): toks = d["usage"].get("completion_tokens", toks)
    dt = time.time() - t0
    res = {"toks": toks, "s": dt, "chunks": chunks, "ttft": (first - t0) if first else None, "text": text}
    if out is not None: out.append(res)
    return res
r = one("Reply with exactly the word: ready", 16)
print(f"[rocm] reply: {r['text'].strip()[:60]!r}")
assert r["toks"] > 0, "no tokens generated"
r = one("Count from 1 to 200 separated by spaces.", 256)
print(f"[rocm] 1 stream: {r['toks']} tok in {r['s']:.1f} s = {r['toks']/r['s']:.1f} tok/s")
r = one("Count from 1 to 100 separated by spaces.", 128, stream=True)
print(f"[rocm] streaming: {r['chunks']} chunks, first token after {r['ttft'] or 0:.2f} s")
assert r["chunks"] > 4, "stream arrived in one burst (fewer than 5 chunks)"
outs = []; ts = [threading.Thread(target=one, args=("Count from 1 to 200 separated by spaces.", 256, False, outs)) for _ in range(4)]
t0 = time.time(); [t.start() for t in ts]; [t.join() for t in ts]; dt = time.time() - t0
tot = sum(o["toks"] for o in outs)
print(f"[rocm] 4 streams: {tot} tok in {dt:.1f} s = {tot/dt:.1f} tok/s total")
PY
say "PASS: HIP build, unit tests, model load, /v1 completion, streaming and 4-stream decode"
