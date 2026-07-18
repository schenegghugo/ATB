#!/usr/bin/env bash
#
# run_ci_checks.sh <build-dir> — the full headless CI gate, runnable locally and
# from any workflow. Runs every demo binary, the fast balance sanity sim, and
# validates the canonical data files. Run from the repo root (the demos and the
# data checks read data/ relative to the CWD).
#
set -euo pipefail

BUILD="${1:-build}"

demos=(
    tb_json_demo tb_enums_demo tb_sha256_demo tb_determinism_demo tb_replay_demo
    tb_replay_source_demo tb_commit_demo tb_password_demo tb_catalog_demo
    tb_creature_demo tb_ruleset_demo tb_map_demo tb_headless tb_build_demo
    tb_spells_demo tb_decoy_demo tb_ai_demo tb_roster_demo tb_event_demo
    tb_pack_demo tb_theme_demo tb_prefs_demo tb_tailscale_demo tb_net_demo tb_matchsource_demo
    tb_loopback_demo tb_net_transport_demo tb_remote_demo tb_account_demo
    tb_arbiter_demo tb_server_demo tb_ranked_demo tb_lobby_demo tb_mailbox_demo
    tb_ranked_rules_demo tb_correspondence_demo tb_lobby_challenge_demo
    tb_lobby_correspondence_demo tb_lobby_forfeit_demo tb_ready_check_demo
    tb_chat_demo tb_spectate_demo tb_chess_clock_demo tb_lobby_chat_demo
    tb_lobby_persist_demo tb_queue_demo
)

for demo in "${demos[@]}"; do
    echo "--- $demo"
    "$BUILD/$demo"
done

echo "--- tb_balance (fixed seed, fast)"
"$BUILD/tb_balance" 200 1

echo "--- data files load + validate (data is canonical, hand-editable)"
"$BUILD/tb_catalog_gen" --check data/catalog.json
"$BUILD/tb_creature_gen" --check data/creatures.json
"$BUILD/tb_ruleset_gen" --check data/rules.json
"$BUILD/tb_ruleset_gen" --check data/rules.ranked.json

echo "ALL CI CHECKS PASS"
