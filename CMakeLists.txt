cmake_minimum_required(VERSION 3.16)
project(TacticalBattler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Version string — the single source of truth is the git tag. CI passes the
# exact release version (-DATB_VERSION=v1.2.3); a bare local build derives it
# from `git describe` (e.g. v1.2.3-4-gabc123 = 4 commits past the tag, or a
# short SHA before the first tag); everything else falls back to "dev". It is
# compiled into the client (see the tactical_battler target) so the running
# binary — and the shipped VERSION.txt — always reports what it actually is.
# ---------------------------------------------------------------------------
if(NOT DEFINED ATB_VERSION OR ATB_VERSION STREQUAL "")
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE ATB_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
    endif()
endif()
if(NOT ATB_VERSION)
    set(ATB_VERSION "dev")
endif()
message(STATUS "ATB version: ${ATB_VERSION}")

# Build the Raylib graphical client. Turn OFF for headless/server/CI builds:
# `core/` + the demos need no graphics, so -DTB_BUILD_GUI=OFF skips Raylib
# entirely (no fetch, no GL/X11) and builds in seconds.
option(TB_BUILD_GUI "Build the Raylib graphical client" ON)

# ---------------------------------------------------------------------------
# Headless gameplay core — no Raylib dependency. This is the heart of the game
# and can be linked into tests / a server build with zero graphics.
# ---------------------------------------------------------------------------
add_library(tb_core STATIC
    src/core/Grid.cpp
    src/core/Battle.cpp
    src/core/AI.cpp
    src/core/Evaluator.cpp
    src/core/Intel.cpp
    src/core/Spells.cpp
    src/core/Creatures.cpp
    src/core/Match.cpp
    src/core/Build.cpp
    src/data/BuildRepository.cpp
    src/data/Json.cpp
    src/data/SpellJson.cpp
    src/data/CatalogJson.cpp
    src/data/CreatureJson.cpp
    src/data/RulesetJson.cpp
    src/data/MapJson.cpp
    src/data/Net.cpp
    src/data/Sha256.cpp
    src/data/Password.cpp
    src/data/Prefs.cpp
    src/net/MatchRunner.cpp
    src/net/Replay.cpp
)
target_include_directories(tb_core PUBLIC src)
target_compile_options(tb_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>
)

# ---------------------------------------------------------------------------
# Headless demos / smoke tests (no window required).
# ---------------------------------------------------------------------------
add_executable(tb_headless tests/headless_demo.cpp)
target_link_libraries(tb_headless PRIVATE tb_core)

add_executable(tb_build_demo tests/build_demo.cpp)
target_link_libraries(tb_build_demo PRIVATE tb_core)

add_executable(tb_spells_demo tests/spells_demo.cpp)
target_link_libraries(tb_spells_demo PRIVATE tb_core)

add_executable(tb_decoy_demo tests/decoy_demo.cpp)
target_link_libraries(tb_decoy_demo PRIVATE tb_core)

add_executable(tb_ai_demo tests/ai_demo.cpp)
target_link_libraries(tb_ai_demo PRIVATE tb_core)

add_executable(tb_roster_demo tests/roster_demo.cpp)
target_link_libraries(tb_roster_demo PRIVATE tb_core)

add_executable(tb_event_demo tests/event_demo.cpp)
target_link_libraries(tb_event_demo PRIVATE tb_core)

add_executable(tb_json_demo tests/json_demo.cpp)
target_link_libraries(tb_json_demo PRIVATE tb_core)

add_executable(tb_enums_demo tests/enums_demo.cpp)
target_link_libraries(tb_enums_demo PRIVATE tb_core)

add_executable(tb_catalog_demo tests/catalog_demo.cpp)
target_link_libraries(tb_catalog_demo PRIVATE tb_core)

add_executable(tb_creature_demo tests/creature_demo.cpp)
target_link_libraries(tb_creature_demo PRIVATE tb_core)

add_executable(tb_ruleset_demo tests/ruleset_demo.cpp)
target_link_libraries(tb_ruleset_demo PRIVATE tb_core)

add_executable(tb_map_demo tests/map_demo.cpp)
target_link_libraries(tb_map_demo PRIVATE tb_core)

add_executable(tb_sha256_demo tests/sha256_demo.cpp)
target_link_libraries(tb_sha256_demo PRIVATE tb_core)

add_executable(tb_determinism_demo tests/determinism_demo.cpp)
target_link_libraries(tb_determinism_demo PRIVATE tb_core)

add_executable(tb_replay_demo tests/replay_demo.cpp)
target_link_libraries(tb_replay_demo PRIVATE tb_core)

add_executable(tb_commit_demo tests/commit_demo.cpp)
target_link_libraries(tb_commit_demo PRIVATE tb_core)

add_executable(tb_password_demo tests/password_demo.cpp)
target_link_libraries(tb_password_demo PRIVATE tb_core)

add_executable(tb_net_demo tests/net_demo.cpp)
target_link_libraries(tb_net_demo PRIVATE tb_core)

add_executable(tb_loopback_demo tests/loopback_demo.cpp)
target_link_libraries(tb_loopback_demo PRIVATE tb_core)

# Generators: write data/catalog.json + data/creatures.json from compiled seeds.
add_executable(tb_catalog_gen tools/catalog_gen.cpp)
target_link_libraries(tb_catalog_gen PRIVATE tb_core)

add_executable(tb_creature_gen tools/creature_gen.cpp)
target_link_libraries(tb_creature_gen PRIVATE tb_core)

add_executable(tb_ruleset_gen tools/ruleset_gen.cpp)
target_link_libraries(tb_ruleset_gen PRIVATE tb_core)

add_executable(tb_brain_arena tests/brain_arena.cpp)
target_link_libraries(tb_brain_arena PRIVATE tb_core)

add_executable(tb_balance tests/balance_sim.cpp)
target_link_libraries(tb_balance PRIVATE tb_core)

add_executable(tb_team_balance tests/team_balance_sim.cpp)
target_link_libraries(tb_team_balance PRIVATE tb_core)

# Presentation-pack manifest loader — raylib-free, so it tests headlessly. The
# texture/draw half (render/SpritePack) is GUI-only and verified in-game.
add_executable(tb_pack_demo tests/pack_demo.cpp src/render/PackManifest.cpp)
target_link_libraries(tb_pack_demo PRIVATE tb_core)

# UI theme loader (render/Theme) — raylib-free like the pack manifest; also
# validates every shipped themes/*.json when run from the repo root (CI gate).
add_executable(tb_theme_demo tests/theme_demo.cpp src/render/Theme.cpp src/render/PackManifest.cpp)
target_link_libraries(tb_theme_demo PRIVATE tb_core)

# Player prefs (settings.json) round-trip + theme/pack discovery scans.
add_executable(tb_prefs_demo tests/prefs_demo.cpp)
target_link_libraries(tb_prefs_demo PRIVATE tb_core)

# The frontend match-driver seam (render/MatchSource) is raylib-free too, so the
# turn-driving logic the GUI runs behind it is verified headless here.
add_executable(tb_matchsource_demo tests/matchsource_demo.cpp src/render/MatchSource.cpp)
target_link_libraries(tb_matchsource_demo PRIVATE tb_core)

# Replay playback source (render/ReplayMatchSource) — raylib-free, headless-tested.
add_executable(tb_replay_source_demo tests/replay_source_demo.cpp)
target_link_libraries(tb_replay_source_demo PRIVATE tb_core)

# ---------------------------------------------------------------------------
# Netcode transport (Phase 4.4) — POSIX TCP + the authoritative 1v1 match
# server/client. Kept OUT of tb_core (which stays portable + socket-free);
# headless, so it builds and tests in the no-GUI CI job.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# The balance sims fan matches out over a thread pool (ATB_JOBS).
target_link_libraries(tb_balance PRIVATE Threads::Threads)
target_link_libraries(tb_team_balance PRIVATE Threads::Threads)

add_library(tb_transport STATIC
    src/net/Socket.cpp
    src/net/GameServer.cpp
    src/net/GameClient.cpp
    src/net/MirrorSession.cpp
    src/net/AccountStore.cpp
    src/net/Arbiter.cpp
    src/net/MailboxRelay.cpp
    src/net/Correspondence.cpp
    src/net/Lobby.cpp
    src/net/Spectate.cpp
    src/net/Subprocess.cpp     # process spawn for the online flight check
    src/net/TailscaleProbe.cpp # tailnet-state probe behind "Play Online"
)
target_link_libraries(tb_transport PUBLIC tb_core)
if(WIN32)
    target_link_libraries(tb_transport PUBLIC ws2_32)  # winsock (Socket.cpp)
    target_link_libraries(tb_transport PUBLIC shell32) # ShellExecute (Subprocess.cpp)
endif()

# Online flight-check probe (parse + step ladder) — pure, fixture-tested headlessly.
add_executable(tb_tailscale_demo tests/tailscale_demo.cpp)
target_link_libraries(tb_tailscale_demo PRIVATE tb_transport)

add_executable(tb_mailbox_demo tests/mailbox_demo.cpp)
target_link_libraries(tb_mailbox_demo PRIVATE tb_transport Threads::Threads)

add_executable(tb_account_demo tests/account_demo.cpp)
target_link_libraries(tb_account_demo PRIVATE tb_transport)

add_executable(tb_arbiter_demo tests/arbiter_demo.cpp)
target_link_libraries(tb_arbiter_demo PRIVATE tb_transport)

add_executable(tb_ranked_rules_demo tests/ranked_rules_demo.cpp)
target_link_libraries(tb_ranked_rules_demo PRIVATE tb_transport)

# CR.6 slice 3: correspondence play + in-game decoy commitments over the relay.
add_executable(tb_correspondence_demo tests/correspondence_demo.cpp)
target_link_libraries(tb_correspondence_demo PRIVATE tb_transport Threads::Threads)

# Phase 4.5 slice 5: seek board + directed challenges → live match.
add_executable(tb_lobby_challenge_demo tests/lobby_challenge_demo.cpp)
target_link_libraries(tb_lobby_challenge_demo PRIVATE tb_transport Threads::Threads)

# Phase 4.5 slice 5 + CR.6: an Unlimited challenge → correspondence, played + ranked
# over the lobby, with a mid-game reconnect.
add_executable(tb_lobby_correspondence_demo tests/lobby_correspondence_demo.cpp)
target_link_libraries(tb_lobby_correspondence_demo PRIVATE tb_transport Threads::Threads)

# Phase 4.5 slice 6.3: idle-clock forfeit → opponent wins + Elo.
add_executable(tb_lobby_forfeit_demo tests/lobby_forfeit_demo.cpp)
target_link_libraries(tb_lobby_forfeit_demo PRIVATE tb_transport Threads::Threads)

# Phase 4.5 slice 6.2: the per-pairing ready check (build at ready, timeout, decline).
add_executable(tb_ready_check_demo tests/ready_check_demo.cpp)
target_link_libraries(tb_ready_check_demo PRIVATE tb_transport Threads::Threads)

# Phase 4.5 slice 6.4: in-match chat relay (async, seat-tagged).
add_executable(tb_chat_demo tests/chat_demo.cpp)
target_link_libraries(tb_chat_demo PRIVATE tb_transport Threads::Threads)

# Phase 5.2: spectate — a watcher mirrors a live match from its logged broadcasts.
add_executable(tb_spectate_demo tests/spectate_demo.cpp)
target_link_libraries(tb_spectate_demo PRIVATE tb_transport Threads::Threads)

# Phase 4.5 slice 6.3 (completion): the true chess clock (bank + increment + flag).
add_executable(tb_chess_clock_demo tests/chess_clock_demo.cpp)
target_link_libraries(tb_chess_clock_demo PRIVATE tb_transport Threads::Threads)

# Phase 4.6: lobby-wide + correspondence-game chat, with the safety levers.
add_executable(tb_lobby_chat_demo tests/lobby_chat_demo.cpp)
target_link_libraries(tb_lobby_chat_demo PRIVATE tb_transport Threads::Threads)

# Persistence: corr games + Mailbox journal survive a restart; clients cold-resume.
add_executable(tb_lobby_persist_demo tests/lobby_persist_demo.cpp)
target_link_libraries(tb_lobby_persist_demo PRIVATE tb_transport Threads::Threads)

# Quick-match queue: auto-pairing on a widening Elo band.
add_executable(tb_queue_demo tests/queue_demo.cpp)
target_link_libraries(tb_queue_demo PRIVATE tb_transport Threads::Threads)

# Team pre-game redesign slice 1: party formation (invite/accept/leave/disband).
add_executable(tb_lobby_party_demo tests/lobby_party_demo.cpp)
target_link_libraries(tb_lobby_party_demo PRIVATE tb_transport Threads::Threads)

# Team pre-game redesign slice 2: team seek/challenge → a draft between two parties.
add_executable(tb_lobby_draft_pairing_demo tests/lobby_draft_pairing_demo.cpp)
target_link_libraries(tb_lobby_draft_pairing_demo PRIVATE tb_transport Threads::Threads)

# Team pre-game redesign slice 3: the snake draft engine (pick loop + reveal + timeout).
add_executable(tb_lobby_draft_engine_demo tests/lobby_draft_engine_demo.cpp)
target_link_libraries(tb_lobby_draft_engine_demo PRIVATE tb_transport Threads::Threads)

# Team pre-game redesign slice 4: completed draft → live team pairing (seat/controller).
add_executable(tb_lobby_draft_finalize_demo tests/lobby_draft_finalize_demo.cpp)
target_link_libraries(tb_lobby_draft_finalize_demo PRIVATE tb_transport Threads::Threads)

# Team pre-game redesign slice 5: a live 2v2 played over the network (champion routing).
add_executable(tb_team_match_demo tests/team_match_demo.cpp)
target_link_libraries(tb_team_match_demo PRIVATE tb_transport Threads::Threads)

# Team pre-game redesign: the draft flow at teamSize 3 (snake order + seat maps at N≠2).
add_executable(tb_lobby_draft_3v3_demo tests/lobby_draft_3v3_demo.cpp)
target_link_libraries(tb_lobby_draft_3v3_demo PRIVATE tb_transport Threads::Threads)

add_executable(tb_server tools/server_main.cpp)
target_link_libraries(tb_server PRIVATE tb_transport)

add_executable(tb_lobby tools/lobby_main.cpp)
target_link_libraries(tb_lobby PRIVATE tb_transport Threads::Threads)
target_compile_definitions(tb_lobby PRIVATE ATB_VERSION="${ATB_VERSION}") # sent in the hello

add_executable(tb_net_transport_demo tests/net_transport_demo.cpp)
target_link_libraries(tb_net_transport_demo PRIVATE tb_transport Threads::Threads)

add_executable(tb_server_demo tests/server_demo.cpp)
target_link_libraries(tb_server_demo PRIVATE tb_transport Threads::Threads)

add_executable(tb_ranked_demo tests/ranked_demo.cpp)
target_link_libraries(tb_ranked_demo PRIVATE tb_transport Threads::Threads)

add_executable(tb_lobby_demo tests/lobby_demo.cpp)
target_link_libraries(tb_lobby_demo PRIVATE tb_transport Threads::Threads)

# The GUI's RemoteMatchSource is header-only + raylib-free, so its exact code path
# is exercised headless here against the real server.
add_executable(tb_remote_demo tests/remote_demo.cpp)
target_link_libraries(tb_remote_demo PRIVATE tb_transport Threads::Threads)

# ---------------------------------------------------------------------------
# Raylib (graphical frontend). Use a system install if present, otherwise fetch.
# Skipped entirely when TB_BUILD_GUI is OFF (headless/server/CI builds).
# ---------------------------------------------------------------------------
if(TB_BUILD_GUI)
find_package(raylib 4.5 QUIET)
if(NOT raylib_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        raylib
        GIT_REPOSITORY https://github.com/raysan5/raylib.git
        GIT_TAG 5.5
        GIT_SHALLOW TRUE
    )
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    # Build GLFW with both display backends so the binary picks Wayland or X11 at
    # runtime. The Wayland backend needs `wayland-protocols` at build time
    # (Arch: `sudo pacman -S wayland-protocols`). Enable it only when present so
    # configuration never hard-fails; otherwise we fall back to X11/XWayland.
    find_package(PkgConfig QUIET)
    set(_tb_wayland OFF)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
        if(WAYLAND_PROTOCOLS_FOUND)
            set(_tb_wayland ON)
        endif()
    endif()
    if(_tb_wayland)
        message(STATUS "Raylib: enabling Wayland + X11 GLFW backends")
    else()
        message(STATUS "Raylib: X11 backend only (install 'wayland-protocols' for native Wayland)")
    endif()
    set(GLFW_BUILD_WAYLAND ${_tb_wayland} CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(raylib)
endif()

add_executable(tactical_battler
    src/main.cpp
    src/render/Renderer.cpp
    src/render/BuildEditorScreen.cpp
    src/render/ConnectScreen.cpp
    src/render/ContentPaths.cpp
    src/render/DraftScreen.cpp
    src/render/FlightCheckScreen.cpp
    src/render/LobbyScreen.cpp
    src/render/MainMenuScreen.cpp
    src/render/MatchSource.cpp
    src/render/PackManifest.cpp
    src/render/PatchNotesScreen.cpp
    src/render/ReadyCheckScreen.cpp
    src/render/SettingsScreen.cpp
    src/render/SpritePack.cpp
    src/render/Theme.cpp
)
target_link_libraries(tactical_battler PRIVATE tb_core tb_transport raylib)
target_compile_definitions(tactical_battler PRIVATE ATB_VERSION="${ATB_VERSION}")

# On Linux, Raylib needs these system libs when built statically.
if(UNIX AND NOT APPLE)
    target_link_libraries(tactical_battler PRIVATE m pthread dl)
endif()
endif() # TB_BUILD_GUI
