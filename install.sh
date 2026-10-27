#!/usr/bin/env bash
# Judas installer. Builds the one binary, installs it to ~/.local/bin, writes a systemd user service and
# launcher entries, and runs config init. It does NOT enable or start the service: judas serve loads the
# whole model into VRAM, so edit ~/.config/judas/config.toml first and start it when the GPU is free.
#
# Env: PREFIX (default ~/.local), JUDAS_PORT (default 8079), and any CMake cache settings via CMAKE_ARGS,
# e.g. CMAKE_ARGS="-DLLAMA_DIR=/opt/llama.cpp -DLLAMA_LIB_DIR=/opt/llama.cpp/build/bin".
set -euo pipefail
cd "$(dirname "$0")"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin"
UNIT="$HOME/.config/systemd/user"
APPS="$HOME/.local/share/applications"
CONF="$HOME/.config/judas"
PORT="${JUDAS_PORT:-8079}"
mkdir -p "$BIN" "$APPS" "$CONF"

for tool in cmake ninja; do
    command -v "$tool" >/dev/null || { echo "[judas] $tool is required" >&2; exit 1; }
done
command -v bwrap >/dev/null || echo "[judas] note: bubblewrap (bwrap) is not installed; shell commands and MCP servers will be refused until it is, or until you set permissions.sandbox = \"none\""

echo "[judas] building (Release)..."
# shellcheck disable=SC2086
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ${CMAKE_ARGS:-} >/dev/null
cmake --build build --target judas

echo "[judas] installing binary -> $BIN/judas"
install -m755 build/judas "$BIN/judas"

if command -v systemctl >/dev/null && systemctl --user show-environment >/dev/null 2>&1; then
    mkdir -p "$UNIT"
    # An existing unit is kept (it may carry your own settings); the stock one goes next to it to compare.
    UNIT_FILE="$UNIT/judas.service"
    if [ -f "$UNIT_FILE" ]; then
        UNIT_FILE="$UNIT/judas.service.new"
        echo "[judas] keeping your $UNIT/judas.service; stock unit written to $UNIT_FILE"
    else
        echo "[judas] writing user service -> $UNIT_FILE"
    fi
    cat > "$UNIT_FILE" <<UNITEOF
[Unit]
Description=Judas, the betrayer harness (model server, agent harness, web app, API)
After=network.target

[Service]
Type=simple
# API tokens (JUDAS_MATRIX_TOKEN and friends) live here, never in config.toml
EnvironmentFile=-$CONF/secrets.env
ExecStart=$BIN/judas serve
Restart=no

[Install]
WantedBy=default.target
UNITEOF
    systemctl --user daemon-reload
    HAVE_UNIT=1
else
    echo "[judas] no systemd user session; run 'judas serve' yourself"
    HAVE_UNIT=0
fi

echo "[judas] launcher entries -> $APPS"
# Omarchy (Hyprland) has its own launch helpers; anything else gets a plain terminal entry and xdg-open.
if command -v omarchy-launch-or-focus >/dev/null; then
    TUI_EXEC="omarchy-launch-or-focus judas \"uwsm-app -- xdg-terminal-exec --app-id=judas --title=JUDAS judas\""
    TUI_TERM=false
    WEB_EXEC="omarchy-launch-webapp \"http://localhost:$PORT\""
else
    TUI_EXEC="judas"
    TUI_TERM=true
    WEB_EXEC="xdg-open http://localhost:$PORT"
fi
cat > "$APPS/judas.desktop" <<DESKEOF
[Desktop Entry]
Type=Application
Name=Judas
GenericName=Local agent harness
Comment=One model, many agents, on your own machine.
Exec=$TUI_EXEC
Terminal=$TUI_TERM
Categories=Utility;Development;
Keywords=judas;ai;agent;local;harness;llm;code;
DESKEOF
cat > "$APPS/judas-web.desktop" <<DESKEOF
[Desktop Entry]
Type=Application
Name=Judas Web
Comment=Judas web app (chat, agent board, memory, snapshots, stats)
Exec=$WEB_EXEC
Terminal=false
StartupNotify=true
DESKEOF
update-desktop-database "$APPS" 2>/dev/null || true

echo "[judas] config init"
touch "$CONF/secrets.env"; chmod 600 "$CONF/secrets.env"
"$BIN/judas" config init || echo "  (config already exists, left as-is)"

cat <<DONE

[judas] installed.
  binary:   $BIN/judas
  config:   $CONF/config.toml   (set [models.main] path to your .gguf, then: judas config validate)
  launcher: "Judas" (terminal UI) and "Judas Web"
DONE
if [ "$HAVE_UNIT" = 1 ]; then
UNIT_STATE="not started"
systemctl --user is-active --quiet judas && UNIT_STATE="running the old binary: systemctl --user restart judas"
cat <<DONE
  service:  judas.service ($UNIT_STATE, $(systemctl --user is-enabled judas 2>/dev/null || echo "not enabled"))

  systemctl --user start judas         # loads the model, serves on 127.0.0.1:$PORT
  judas                                # terminal UI
  xdg-open http://localhost:$PORT      # web app
DONE
fi
