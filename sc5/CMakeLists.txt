# sc5 - Soulcalibur V (Xbox 360), recompiled with ReXGlue.
#
# SDK boilerplate lives in generated/rexglue.cmake. The SDK: CMAKE_PREFIX_PATH=..\external\sdk (or REXSDK_DIR
# for a source tree); tools\devenv.cmd sets up clang, CMake and Ninja.

cmake_minimum_required(VERSION 3.25)
project(sc5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Every player's PC builds its own copy, with or without AVX2: no fused multiply-adds the source did not ask for,
# so every build computes the same floats (netplay compares the games' state). As MK9 Online.
add_compile_options(-ffp-contract=off)

# the game's release (sc5\VERSION): netplay refuses to match two games built from different ones
if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
    file(READ "${CMAKE_SOURCE_DIR}/VERSION" SC5_VERSION)
    string(STRIP "${SC5_VERSION}" SC5_VERSION)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
endif()
if(NOT SC5_VERSION)
    set(SC5_VERSION "0.0.0")
endif()
message(STATUS "SC5 game version ${SC5_VERSION}")

include(generated/rexglue.cmake)

set(SC5_SOURCES
    src/main.cpp
    src/sc5_app.cpp
    src/sc5/battle.cpp
    src/sc5/frame.cpp
    src/sc5/game.cpp
    src/sc5/guards.cpp
    src/sc5/input.cpp
    src/sc5/system_ui.cpp
    src/sc5/testinput.cpp
    src/sc5/unlocks.cpp
    src/sc5/net/synctest.cpp
    src/sc5/debug_watch.cpp
    src/sc5/users.cpp
    src/sc5/rng.cpp
    src/sc5/net/netplay.cpp
    src/sc5/sound.cpp
    src/sc5/dlc.cpp
    src/sc5/select.cpp
    src/sc5/creation.cpp
    src/sc5/creation_check.cpp
    src/sc5/versus.cpp
    src/sc5/autopilot.cpp
    src/sc5/link.cpp
    src/sc5/sparring.cpp
    src/sc5/controls.cpp
    src/sc5/display.cpp
    src/sc5/framemeter.cpp
    src/sc5/rerun_profile.cpp
    src/sc5/result_overlay.cpp
    src/sc5/hud.cpp
    src/sc5/mirror.cpp
    src/sc5/ui_link.cpp
    src/sc5/online_menu.cpp
    src/sc5/online_draw.cpp
)

# The player package's ready-made sc5.exe: everything but the game's code, which each player's PC compiles from their
# own disc into sc5game.dll with nothing but the package (sc5/player/README.md). tools\build-player-host.cmd builds it.
option(SC5_PLAYER_HOST "Build sc5.exe without the game's code, for the player package" OFF)
if(SC5_PLAYER_HOST)
    list(APPEND SC5_SOURCES player/guestlog.cpp)
endif()

if(WIN32)
    add_executable(sc5 WIN32 ${SC5_SOURCES})
else()
    add_executable(sc5 ${SC5_SOURCES})
endif()

if(SC5_PLAYER_HOST)
    # rexglue_setup_target without the translated code (no sc5_recomp, no codegen run): sc5game.dll has it
    target_include_directories(sc5 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${REXGLUE_ENTRYPOINT_INCLUDE_DIR})
    target_link_libraries(sc5 PRIVATE rex::runtime)
    rexglue_configure_target(sc5 GPU_PLUGINS xenos)
    target_compile_definitions(sc5 PRIVATE SC5_PLAYER_HOST=1)
    # what sc5.exe and sc5game.dll take from each other, read from this build's objects just before it links
    set(SC5_LINK_DIR "${CMAKE_CURRENT_BINARY_DIR}/player")
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    get_filename_component(SC5_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET sc5 PRE_LINK
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/player/boundary.py"
            --llvm "${SC5_LLVM_BIN}" --manifest "${CMAKE_CURRENT_SOURCE_DIR}/sc5_manifest.toml"
            --out "${SC5_LINK_DIR}" "$<TARGET_OBJECTS:sc5>"
        COMMAND_EXPAND_LISTS VERBATIM)
    target_link_options(sc5 PRIVATE "LINKER:/DEF:${SC5_LINK_DIR}/host_exports.def" "LINKER:${SC5_LINK_DIR}/sc5game.lib")
else()
    rexglue_setup_target(sc5 GPU_PLUGINS xenos)
endif()
target_compile_definitions(sc5 PRIVATE SC5_VERSION="${SC5_VERSION}")

# The check of created characters from another PC, tested alone with the game's own records and broken ones:
# out\build\win-amd64-release\creation_check_test.exe tests\data (exit code 0: every test passed)
add_executable(creation_check_test tests/creation_check_test.cpp src/sc5/creation_check.cpp)
target_include_directories(creation_check_test PRIVATE src)
target_compile_definitions(creation_check_test PRIVATE _CRT_SECURE_NO_WARNINGS)
