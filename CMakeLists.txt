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
    src/core/Build.cpp
    src/data/BuildRepository.cpp
    src/data/Json.cpp
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

add_executable(tb_json_demo tests/json_demo.cpp)
target_link_libraries(tb_json_demo PRIVATE tb_core)

add_executable(tb_balance tests/balance_sim.cpp)
target_link_libraries(tb_balance PRIVATE tb_core)

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
)
target_link_libraries(tactical_battler PRIVATE tb_core raylib)

# On Linux, Raylib needs these system libs when built statically.
if(UNIX AND NOT APPLE)
    target_link_libraries(tactical_battler PRIVATE m pthread dl)
endif()
endif() # TB_BUILD_GUI
