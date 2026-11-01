#!/usr/bin/env bash
# Swarm CPU benchmark: aggregate and per-request tok/s for parallel = 1,2,4,8 at several core counts, plus a
# serialized baseline (8 requests one after another). Starts a TEMPORARY swarm instance from this build on port
# 18083 (never touches ~/.config/judas or the real units), changes its cores live through POST /api/cores, writes
# a markdown table. CPU only. Run it on an idle machine: it uses up to every core.
#
#   tools/bench-swarm.sh [--cores "4 6 8 12 16 24"] [--parallel "1 2 4 8"] [--max-tokens 256]
#                        [--baseline-cores "12"] [--out docs/SWARM-BENCH.md] [--model PATH] [--no-pin] [--force]
#   quick check: tools/bench-swarm.sh --cores 4 --parallel 1 --baseline-cores "" --max-tokens 32 --out /tmp/x.md
#
# Thinking tokens count as generated tokens (the server reports completion_tokens). Identical prompts, so prefill
# is shared; this measures decode. Refuses to start when the 1-minute load average is 4 or more (--force overrides)
# and records the load in the output.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${BIN:-$here/build/judas}"
MODEL="${MODEL:-$HOME/models/gguf/lfm2.5-2.6b/LFM2.5-2.6B-QAD-Q4_0.gguf}"
CORES="4 6 8 12 16 24"; PAR="1 2 4 8"; MAXTOK=256; BASE_CORES="12"
OUT="$here/docs/SWARM-BENCH.md"; PIN=true; FORCE=0; PORT=18083
while [ $# -gt 0 ]; do
  case "$1" in
    --cores) CORES="$2"; shift 2;;
    --parallel) PAR="$2"; shift 2;;
    --max-tokens) MAXTOK="$2"; shift 2;;
    --baseline-cores) BASE_CORES="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    --model) MODEL="$2"; shift 2;;
    --no-pin) PIN=false; shift;;
    --force) FORCE=1; shift;;
    *) echo "unknown option $1" >&2; exit 2;;
  esac
done
[ -x "$BIN" ] || { echo "build first: $BIN missing" >&2; exit 1; }
[ -f "$MODEL" ] || { echo "model not found: $MODEL" >&2; exit 1; }
load="$(cut -d' ' -f1 /proc/loadavg)"
if [ "$FORCE" != 1 ] && awk -v l="$load" 'BEGIN{exit !(l>=4)}'; then
  echo "load average $load is 4 or more; the numbers would be wrong. Wait, or pass --force." >&2; exit 1
fi
work="$(mktemp -d "$here/build/bench-swarm/run.XXXXXX" 2>/dev/null || { mkdir -p "$here/build/bench-swarm"; mktemp -d "$here/build/bench-swarm/run.XXXXXX"; })"
first="${CORES%% *}"
cat > "$work/swarm.toml" <<TOML
data_dir = "$work/data"
[engine]
model = "bench"
max_agents = 8
pin_cores = $PIN
[models.bench]
path = "$MODEL"
gpu_layers = 0
kv_type = "f16"
ctx_total = 65536
ctx_per_agent = 8192
threads = $first
mmproj = "none"
[models.bench.sampling]
temp = 0.0
top_k = 1
[server]
port = $PORT
TOML
URL="http://127.0.0.1:$PORT"
JUDAS_CONFIG="$work/swarm.toml" "$BIN" serve > "$work/serve.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true; rm -rf "$work"' EXIT
for _ in $(seq 1 120); do curl -fsS "$URL/health" >/dev/null 2>&1 && break; kill -0 $pid 2>/dev/null || { cat "$work/serve.log" >&2; exit 1; }; sleep 1; done
curl -fsS "$URL/health" >/dev/null || { echo "instance did not come up" >&2; exit 1; }
setcores() {
  curl -fsS -X POST "$URL/api/cores" -H 'Content-Type: application/json' -d "{\"threads\":$1,\"pin\":$PIN}" >/dev/null
}
drive() { python3 -I "$here/tools/bench-swarm.py" "$URL" --max-tokens "$MAXTOK" "$@"; }
now() { date '+%Y-%m-%d %H:%M'; }
{
  echo "# Swarm CPU benchmark"
  echo
  echo "Run $(now) on $(hostname), $(nproc) logical CPUs. Model $(basename "$MODEL"), max_tokens $MAXTOK, temperature 0, pinning $PIN, 1-minute load average at start: $load."
  echo "Thinking tokens count as generated tokens. Numbers are aggregate tok/s across all requests (per-request mean in brackets)."
  echo
  echo "| cores | $(for p in $PAR; do printf 'parallel %s | ' "$p"; done)"
  echo "|---|$(for p in $PAR; do printf -- '---|'; done)"
} > "$OUT"
cell() { python3 -c 'import json,sys; j=json.loads(sys.argv[1]); print("%s (%s)%s" % (j["agg_tps"], j["per_req_tps"], " ERR" if j["errors"] else ""))' "$1"; }
for c in $CORES; do
  setcores "$c"
  drive --parallel 1 >/dev/null   # warm up: pages the model in on the new core set
  row="| $c |"
  for p in $PAR; do
    res="$(drive --parallel "$p")"; echo "cores $c parallel $p: $res" >&2
    row="$row $(cell "$res") |"
  done
  echo "$row" >> "$OUT"
done
if [ -n "$BASE_CORES" ]; then
  { echo; echo "## Serialized baseline (8 requests one after another)"; echo
    echo "| cores | serialized tok/s | 8 parallel tok/s | batching gain |"; echo "|---|---|---|---|"; } >> "$OUT"
  for c in $BASE_CORES; do
    setcores "$c"; drive --parallel 1 >/dev/null
    s="$(drive --parallel 8 --serial)"; echo "cores $c serial 8: $s" >&2
    p8="$(drive --parallel 8)"; echo "cores $c parallel 8: $p8" >&2
    python3 -c 'import json,sys; s=json.loads(sys.argv[1]); p=json.loads(sys.argv[2]); print("| %s | %s | %s | %.1fx |" % (sys.argv[3], s["agg_tps"], p["agg_tps"], p["agg_tps"]/s["agg_tps"] if s["agg_tps"] else 0))' "$s" "$p8" "$c" >> "$OUT"
  done
fi
echo "wrote $OUT" >&2
