#!/usr/bin/env bash
#
# balance.sh — run the AI-vs-AI balance simulator with friendly options.
#
# Reads data/catalog.json + data/creatures.json + a rules file (default: the
# RANKED ruleset, data/rules.ranked.json — balance work targets competitive
# play; pass --rules rules.json for the casual set). Builds tb_balance if
# needed, then runs it.
#
set -euo pipefail
cd "$(dirname "$0")/.." # repo root (this script lives in scripts/)

matches=""
seed=42
out="output/balance_report.txt"
map=""
map_set=0
data=""
team=""
brain="adaptive"
jobs=""
rules="rules.ranked.json"

usage() {
    cat <<'EOF'
Usage: scripts/balance.sh [options]

  -n, --matches N     number of matches (default: 3000, or 300 with a minimax
                      brain — deep/adaptive run ~1000x slower than beam)
  -s, --seed S        RNG seed (deterministic)      (default 42)
  -o, --out FILE      report output file            (default output/balance_report.txt)
  -m, --map KEY|FILE  battlefield: a map key under data/maps/ (e.g. 'duel'),
                      a path to a map .json, or '' to force a random arena
  -d, --data DIR      data directory holding catalog/creatures/rules (+ maps/)
  -r, --rules FILE    rules file: a name inside the data dir or a path
                      (default rules.ranked.json — the competitive ruleset)
  -t, --team N        team size: 1 = 1v1, 2 = 2v2, 3 = 3v3 (default: from the rules file)
  -b, --brain NAME    AI both sides play (default 'adaptive' — the latest AI:
                      turn-level minimax + observed-opponent intel).
                      Roster: adaptive | deep (minimax, omniscient) |
                      scout (intel beam) | beam | greedy.
                      Per-core throughput (1v1): beam/greedy ~1000+ matches/s,
                      scout ~150, deep/adaptive ~0.6; matches fan out over all
                      cores (deterministic — identical report at any -j).
  -j, --jobs N        worker threads (default: all cores)
  -h, --help          show this help

Examples:
  scripts/balance.sh                             # 300 matches, adaptive AI
  scripts/balance.sh -n 1000                     # bigger adaptive study (~30 min)
  scripts/balance.sh --brain scout -n 30000      # intel realism at beam speed
  scripts/balance.sh --brain beam -n 30000 -s 42 # the classic fast sweep
  scripts/balance.sh --map duel
  scripts/balance.sh --rules rules.json          # casual/default ruleset
  scripts/balance.sh --team 2 -n 10000 --brain scout
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
        -r | --rules) rules="$2"; shift 2 ;;
        -t | --team) team="$2"; shift 2 ;;
        -b | --brain) brain="$2"; shift 2 ;;
        -j | --jobs) jobs="$2"; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "balance.sh: unknown option '$1'" >&2; usage; exit 1 ;;
    esac
done

# Match-count default is brain-aware: the minimax brains (deep/adaptive) play
# ~1000x slower than the beam family, so an unset -n means a study-sized run
# there (~3 min: ~0.6 matches/s per core) and a sweep-sized one otherwise.
eff_jobs="${jobs:-$(nproc)}"
if [[ -z "$matches" ]]; then
    case "$brain" in
        deep | adaptive) matches=$((100 * eff_jobs)) ;;
        *) matches=3000 ;;
    esac
fi

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
[[ -n "$brain" ]] && export ATB_BRAIN="$brain"
[[ -n "$jobs" ]] && export ATB_JOBS="$jobs"
export ATB_RULES="$rules"

echo "balance: ${matches} matches, seed ${seed}, rules '${rules}', ${eff_jobs} thread(s)${team:+, ${team}v${team}}${map:+, map='${map}'}${brain:+, brain='${brain}'}${data:+, data='${data}'}"
case "$brain" in
    deep | adaptive)
        echo "balance: note: '${brain}' plays ~0.6 matches/s per core in 1v1 — this run is roughly $((matches * 10 / (6 * eff_jobs) / 60 + 1)) min; use --brain scout for fast sweeps" ;;
esac
./build/tb_balance "$matches" "$seed" "$out"
base="${out%.txt}"
echo "balance: wrote ${out}, ${base}.html (charts), and ${base}.{spells,pairs,length,outcomes}.csv"
