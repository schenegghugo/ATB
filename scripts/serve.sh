#!/usr/bin/env bash
#
# serve.sh — one-shot host launcher for online play over Tailscale.
#
# Pulls the latest committed code, builds the two headless server daemons, and
# runs them BOUND TO THIS MACHINE'S TAILNET IP (never the public internet):
#
#   tb_lobby  (port 5555) — the GUI "Play Online" Online Home: seeks, challenges,
#                           live + correspondence games. THIS is what friends use.
#   tb_server (port 5556) — the direct 1v1 ConnectScreen queue (optional; the GUI
#                           "Play Online" flow does NOT use it).
#
# Run from anywhere in the repo:  ./scripts/serve.sh
# Override ports with env vars:    LOBBY_PORT=5555 SERVER_PORT=5556 ./scripts/serve.sh
# Lobby only (skip tb_server):     NO_SERVER=1 ./scripts/serve.sh
#
set -euo pipefail
cd "$(dirname "$0")/.."   # repo root, regardless of where it's invoked from

LOBBY_PORT="${LOBBY_PORT:-5555}"   # match the GUI's default "Play Online" port
SERVER_PORT="${SERVER_PORT:-5556}" # tb_server's own port (must differ from lobby)
BUILD="${BUILD:-build}"

# 1. Latest committed code. --ff-only fails loudly on divergence instead of
#    creating a merge or clobbering local state (build/ and lobby-state/ are
#    gitignored, so they never dirty the tree).
echo "== git pull =="
git pull --ff-only

# 2. Configure (first run only) + build both daemons. Headless: no GUI deps, so
#    a bare server box needs no raylib/X/Wayland.
if [[ ! -d "$BUILD" ]]; then
    cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DTB_BUILD_GUI=OFF
fi
echo "== build tb_lobby + tb_server =="
cmake --build "$BUILD" --target tb_lobby tb_server -j

# 3. This machine's tailnet address — clients connect to exactly this. Binding
#    here (not 0.0.0.0) is the whole point: reachable only over the tailnet.
TIP="$(tailscale ip -4 2>/dev/null | head -n1 || true)"
if [[ -z "$TIP" ]]; then
    echo "serve.sh: no tailnet IP — is the daemon up?  sudo tailscale up" >&2
    echo "          (after a kernel update, reboot first — see CONNECT.md)"   >&2
    exit 1
fi

# 4. Run. tb_server (if enabled) goes to the background; tb_lobby stays in the
#    foreground so a single Ctrl-C stops everything (the trap kills the server).
if [[ -z "${NO_SERVER:-}" ]]; then
    "$BUILD/tb_server" "$SERVER_PORT" "$TIP" data/rules.ranked.json &
    SERVER_PID=$!
    # NOT exec'd below, so this trap survives to stop the background server when
    # tb_lobby exits or you Ctrl-C.
    trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT INT TERM
    echo "== tb_server on $TIP:$SERVER_PORT (direct 1v1) =="
fi

echo "== tb_lobby  on $TIP:$LOBBY_PORT (friends: GUI → Play Online → host $TIP:$LOBBY_PORT) =="
echo "   Ctrl-C to stop."
"$BUILD/tb_lobby" "$LOBBY_PORT" "$TIP" data/rules.json data/rules.ranked.json
