@echo off
rem The player package's ready-made sc5.exe: the game's own code stays out of it, each player's PC compiles that part
rem from their own disc with nothing but the package (sc5\player\README.md). Built against external\sdk, the runtime the
rem package ships, for any CPU the game runs on. Needs sc5\generated\default (sc5\codegen.cmd) and Python (the
rem boundary between sc5.exe and sc5game.dll is read from sc5.exe's objects, tools\player\boundary.py).
rem Result in the build folder (default dist\host): sc5.exe, sc5.lib, player\ (game_link.rsp: the player's link).
rem Usage: tools\build-player-host.cmd [<build folder>]
setlocal
call "%~dp0devenv.cmd"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%SC5_ROOT%\dist\host"
for %%F in ("%OUT%") do set "OUT=%%~fF"
cd /d "%SC5_ROOT%\sc5" || exit /b 1
if not exist "generated\default\sources.cmake" (
  echo No sc5\generated\default: run sc5\codegen.cmd first.
  exit /b 1
)
if not exist "%OUT%\build.ninja" (
  cmake -S . -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" ^
    "-DCMAKE_PREFIX_PATH=%SC5_ROOT%\external\sdk" -DSC5_PLAYER_HOST=ON || exit /b 1
)
cmake --build "%OUT%" || exit /b 1
echo BUILD OK
