cmake_minimum_required(VERSION 3.16)
project(TacticalBattler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

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
    src/net/MatchRunner.cpp
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

add_executable(tb_balance tests/balance_sim.cpp)
target_link_libraries(tb_balance PRIVATE tb_core)

add_executable(tb_team_balance tests/team_balance_sim.cpp)
target_link_libraries(tb_team_balance PRIVATE tb_core)

# Presentation-pack manifest loader — raylib-free, so it tests headlessly. The
# texture/draw half (render/SpritePack) is GUI-only and verified in-game.
add_executable(tb_pack_demo tests/pack_demo.cpp src/render/PackManifest.cpp)
target_link_libraries(tb_pack_demo PRIVATE tb_core)

# The frontend match-driver seam (render/MatchSource) is raylib-free too, so the
# turn-driving logic the GUI runs behind it is verified headless here.
add_executable(tb_matchsource_demo tests/matchsource_demo.cpp src/render/MatchSource.cpp)
target_link_libraries(tb_matchsource_demo PRIVATE tb_core)

# ---------------------------------------------------------------------------
# Netcode transport (Phase 4.4) — POSIX TCP + the authoritative 1v1 match
# server/client. Kept OUT of tb_core (which stays portable + socket-free);
# headless, so it builds and tests in the no-GUI CI job.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(tb_transport STATIC
    src/net/Socket.cpp
    src/net/GameServer.cpp
    src/net/GameClient.cpp
    src/net/MirrorSession.cpp
)
target_link_libraries(tb_transport PUBLIC tb_core)

add_executable(tb_server tools/server_main.cpp)
target_link_libraries(tb_server PRIVATE tb_transport)

add_executable(tb_net_transport_demo tests/net_transport_demo.cpp)
target_link_libraries(tb_net_transport_demo PRIVATE tb_transport Threads::Threads)

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
    src/render/ContentPaths.cpp
    src/render/MatchSource.cpp
    src/render/PackManifest.cpp
    src/render/SpritePack.cpp
)
target_link_libraries(tactical_battler PRIVATE tb_core tb_transport raylib)

# On Linux, Raylib needs these system libs when built statically.
if(UNIX AND NOT APPLE)
    target_link_libraries(tactical_battler PRIVATE m pthread dl)
endif()
endif() # TB_BUILD_GUI
