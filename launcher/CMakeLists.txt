cmake_minimum_required(VERSION 3.21)
project(sc5_launcher VERSION 0.1.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

# SDL3 for controllers (GamepadNav), static so it links into the exe, from a copy of its sources on this PC: nothing is
# downloaded. launcher\build.cmd finds one (SDL3_DIR, else external\sdl3, else MK3D Online's build cache).
set(SC5_SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../external/sdl3" CACHE PATH "SDL3 sources")
if(NOT EXISTS "${SC5_SDL3_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "SDL3 sources not found in ${SC5_SDL3_DIR}: set -DSC5_SDL3_DIR=<SDL3 source folder> "
                        "(see docs/LAUNCHER.md)")
endif()
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory("${SC5_SDL3_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/sdl3" EXCLUDE_FROM_ALL)

qt_add_executable(SC5Launcher WIN32
    src/main.cpp
    src/AppPaths.cpp src/AppPaths.h
    src/WinUtil.cpp src/WinUtil.h
    src/GameData.cpp src/GameData.h
    src/Style.cpp src/Style.h
    src/ServerClient.cpp src/ServerClient.h
    src/UdpRendezvous.cpp src/UdpRendezvous.h
    src/TunnelBridge.cpp src/TunnelBridge.h
    src/GameProcess.cpp src/GameProcess.h
    src/MatchController.cpp src/MatchController.h
    src/FeedLink.cpp src/FeedLink.h
    src/ReportSender.cpp src/ReportSender.h
    src/GamepadNav.cpp src/GamepadNav.h
    src/Widgets.cpp src/Widgets.h
    src/Dialogs.cpp src/Dialogs.h
    src/PlayPage.cpp src/PlayPage.h
    src/MenuBridge.cpp src/MenuBridge.h
    src/TestDriver.cpp src/TestDriver.h
    src/ToolPages.cpp src/ToolPages.h
    src/SupportPage.cpp src/SupportPage.h
    src/Controls.cpp src/Controls.h
    src/KeyboardBox.cpp src/KeyboardBox.h
    src/X360.cpp src/X360.h
    src/Upnp.cpp src/Upnp.h
    src/Updater.cpp src/Updater.h
    src/MainWindow.cpp src/MainWindow.h
    data/logo/SC5Launcher.rc  # the exe's icon
)
# its version and publisher, as Windows and antivirus programs read them (data/version.rc.in)
configure_file(data/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/SC5Launcher-version.rc" @ONLY)
target_sources(SC5Launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/SC5Launcher-version.rc")

qt_add_resources(SC5Launcher "gamedata"
    PREFIX "/data"
    BASE data
    FILES data/sc5.json
)

# SC5 Online's emblem: the header and dialogs (sc5-logo.png) and the window icon (icon-N.png); tools/make_art.py makes
# them and sc5.ico
qt_add_resources(SC5Launcher "logo"
    PREFIX "/logo"
    BASE data/logo
    FILES data/logo/sc5-logo.png data/logo/icon-16.png data/logo/icon-24.png data/logo/icon-32.png
          data/logo/icon-48.png data/logo/icon-64.png data/logo/icon-128.png data/logo/icon-256.png
)

# the ranks' badges by ranked title (Soul Warrior: soul-warrior.png), lowest first as the server's ladder
# (server/elo.py); tools/make_art.py draws them
qt_add_resources(SC5Launcher "ranks"
    PREFIX "/ranks"
    BASE data/ranks
    FILES data/ranks/novice.png data/ranks/apprentice.png data/ranks/squire.png data/ranks/wanderer.png
          data/ranks/duelist.png data/ranks/knight.png data/ranks/vanguard.png data/ranks/blademaster.png
          data/ranks/champion.png data/ranks/conqueror.png data/ranks/hero.png data/ranks/legend.png
          data/ranks/soul-warrior.png data/ranks/soul-master.png data/ranks/spirit-sword.png data/ranks/eternal-soul.png
)

qt_add_resources(SC5Launcher "sounds"
    PREFIX "/sounds"
    BASE data/sounds
    FILES data/sounds/match-found.wav  # MK3D Online's synthesized chime (nothing recorded): a match found, over the game
)

target_compile_definitions(SC5Launcher PRIVATE
    SC5_LAUNCHER_VERSION="${PROJECT_VERSION}"
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(SC5Launcher PRIVATE Qt6::Widgets Qt6::Network SDL3::SDL3-static dwmapi advapi32 winmm)
set_target_properties(SC5Launcher PROPERTIES OUTPUT_NAME "SC5Launcher")

if(MSVC)
    target_compile_options(SC5Launcher PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
endif()

# Copy the Qt runtime next to the exe:  cmake --build <dir> --target deploy
find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
if(WINDEPLOYQT)
    add_custom_target(deploy
        COMMAND "${WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw
                --no-compiler-runtime "$<TARGET_FILE:SC5Launcher>"
        DEPENDS SC5Launcher
        COMMENT "windeployqt SC5Launcher"
        VERBATIM)
endif()
