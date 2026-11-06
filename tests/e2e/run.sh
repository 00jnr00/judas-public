#!/usr/bin/env bash
# Browser end-to-end tests for the web UI. Starts the offline fake model server and a headless judas
# (own temp config and data dir, never the live one), runs web.test.js, stops both by PID.
# Usage: tests/e2e/run.sh [path/to/judas]     env: JUDAS_PORT FAKE_PORT ONLY CHROMIUM PLAYWRIGHT_DIR
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
bin="${1:-$root/build/judas}"
jport="${JUDAS_PORT:-18179}"
fport="${FAKE_PORT:-18181}"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/judas-e2e.XXXXXX")"
pids=()
cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
  for p in "${pids[@]}"; do wait "$p" 2>/dev/null; done
  rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

[ -x "$bin" ] || { echo "no judas binary at $bin (build first)"; exit 2; }
cat > "$tmp/config.toml" <<TOML
data_dir = "$tmp/data"
[engine]
model = "fake"
[models.fake]
url = "http://127.0.0.1:$fport"
remote_model = "fake-model"
[server]
bind = "127.0.0.1"
port = $jport
[projects]
root = "$tmp/projects"
TOML

FAKE_PORT="$fport" python3 "$here/fake_openai.py" >"$tmp/fake.log" 2>&1 &
pids+=($!)
JUDAS_CONFIG="$tmp/config.toml" "$bin" serve >"$tmp/judas.log" 2>&1 &
pids+=($!)

for _ in $(seq 1 100); do
  curl -sf "http://127.0.0.1:$jport/api/state" >/dev/null 2>&1 && break
  sleep 0.2
done
curl -sf "http://127.0.0.1:$jport/api/state" >/dev/null 2>&1 || { echo "judas did not start"; cat "$tmp/judas.log"; exit 2; }

BASE="http://127.0.0.1:$jport" node "$here/web.test.js"
rc=$?
[ $rc -ne 0 ] && { echo "--- judas log tail"; tail -20 "$tmp/judas.log"; }
exit $rc
