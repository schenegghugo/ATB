#!/usr/bin/env bash
#
# balance.sh — run the AI-vs-AI balance simulator with friendly options.
#
# Reads data/catalog.json + data/creatures.json + data/rules.json (the same files
# the game uses). Builds tb_balance if needed, then runs it.
#
set -euo pipefail
cd "$(dirname "$0")/.." # repo root (this script lives in scripts/)

matches=3000
seed=42
out="output/balance_report.txt"
map=""
map_set=0
data=""
team=""

usage() {
    cat <<'EOF'
Usage: scripts/balance.sh [options]

  -n, --matches N     number of matches            (default 3000)
  -s, --seed S        RNG seed (deterministic)      (default 42)
  -o, --out FILE      report output file            (default output/balance_report.txt)
  -m, --map KEY|FILE  battlefield: a map key under data/maps/ (e.g. 'duel'),
                      a path to a map .json, or '' to force a random arena
  -d, --data DIR      data directory holding catalog/creatures/rules (+ maps/)
  -t, --team N        team size: 1 = 1v1, 2 = 2v2, 3 = 3v3 (default: from rules.json)
  -h, --help          show this help

Examples:
  scripts/balance.sh -n 30000 -s 42
  scripts/balance.sh --map duel
  scripts/balance.sh --team 2 -n 10000
  scripts/balance.sh --map data/maps/duel.json -n 10000
  scripts/balance.sh --data /tmp/mymod --map arena -o mymod_report.txt
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n | --matches) matches="$2"; shift 2 ;;
        -s | --seed) seed="$2"; shift 2 ;;
        -o | --out) out="$2"; shift 2 ;;
        -m | --map) map="$2"; map_set=1; shift 2 ;;
        -d | --data) data="$2"; shift 2 ;;
        -t | --team) team="$2"; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "balance.sh: unknown option '$1'" >&2; usage; exit 1 ;;
    esac
done

# Build the simulator (configure first if the build dir isn't set up).
if [[ ! -f build/CMakeCache.txt ]]; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTB_BUILD_GUI=OFF
fi
cmake --build build --target tb_balance -j >/dev/null

[[ -n "$data" ]] && export ATB_DATA_DIR="$data"
# Export ATB_MAP only when --map was given. It may be "", which forces a random
# arena even if rules.json names a map.
[[ "$map_set" == 1 ]] && export ATB_MAP="$map"
[[ -n "$team" ]] && export ATB_TEAM="$team"

echo "balance: ${matches} matches, seed ${seed}${team:+, ${team}v${team}}${map:+, map='${map}'}${data:+, data='${data}'}"
./build/tb_balance "$matches" "$seed" "$out"
base="${out%.txt}"
echo "balance: wrote ${out}, ${base}.html (charts), and ${base}.{spells,pairs,length,outcomes}.csv"
