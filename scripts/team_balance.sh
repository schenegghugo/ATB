#!/usr/bin/env bash
#
# team_balance.sh — run the NvN team-composition balance simulator.
#
# Sibling to balance.sh. Where balance.sh reports per-spell stats, this classifies
# every build into an archetype (Aggro/Control/Summoner/Support/Evasion) and reports
# win rates by team composition plus a composition-vs-composition matchup matrix.
#
# Reads the same data/catalog.json + data/creatures.json + data/rules.json the game
# uses. Builds tb_team_balance if needed, then runs it. Meant for 2v2 / 3v3.
#
set -euo pipefail
cd "$(dirname "$0")/.." # repo root (this script lives in scripts/)

matches=40000
seed=42
out="output/team_report.txt"
team=2
map=""
map_set=0
data=""

usage() {
    cat <<'EOF'
Usage: scripts/team_balance.sh [options]

  -n, --matches N     number of matches            (default 4000)
  -s, --seed S        RNG seed (deterministic)      (default 42)
  -o, --out FILE      report output file            (default output/team_report.txt)
  -t, --team N        team size: 2 = 2v2, 3 = 3v3   (default 2)
  -m, --map KEY|FILE  battlefield: a map key under data/maps/ (e.g. 'duel'),
                      a path to a map .json, or '' to force a random arena
  -d, --data DIR      data directory holding catalog/creatures/rules (+ maps/)
  -h, --help          show this help

Examples:
  scripts/team_balance.sh --team 2 -n 8000
  scripts/team_balance.sh --team 3 --map duel
  scripts/team_balance.sh -t 2 --data /tmp/mymod -o mymod_team.txt
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -n | --matches) matches="$2"; shift 2 ;;
        -s | --seed) seed="$2"; shift 2 ;;
        -o | --out) out="$2"; shift 2 ;;
        -t | --team) team="$2"; shift 2 ;;
        -m | --map) map="$2"; map_set=1; shift 2 ;;
        -d | --data) data="$2"; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "team_balance.sh: unknown option '$1'" >&2; usage; exit 1 ;;
    esac
done

# Build the simulator (configure first if the build dir isn't set up).
if [[ ! -f build/CMakeCache.txt ]]; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTB_BUILD_GUI=OFF
fi
cmake --build build --target tb_team_balance -j >/dev/null

[[ -n "$data" ]] && export ATB_DATA_DIR="$data"
[[ "$map_set" == 1 ]] && export ATB_MAP="$map"
export ATB_TEAM="$team"

echo "team_balance: ${matches} matches, seed ${seed}, ${team}v${team}${map:+, map='${map}'}${data:+, data='${data}'}"
./build/tb_team_balance "$matches" "$seed" "$out"
base="${out%.txt}"
echo "team_balance: wrote ${out}, ${base}.html (charts), and ${base}.{roles,comps,matchups}.csv"
