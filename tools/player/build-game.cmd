@echo off
rem Builds the native Soulcalibur V on this PC from the player's own disc and title update (nobody ships the game's
rem code: every PC translates its own copy). The launcher's Build game page runs it once the disc and the update are in
rem place (docs\PROTOCOL.md 3.7); it can be run by hand too. Nothing has to be installed: everything used here comes
rem with the package (sc5\player\README.md in the repository).
rem   SC5_GAME_DIR   the extracted disc: default.xex with title update 3's default.xexp beside it (default: disc\ here)
rem   SC5_GAME_EXE   where sc5.exe goes; sc5game.dll and the runtime's DLLs go beside it (default: sc5.exe here)
rem   (SC5_UPDATE_DIR, the update's other files, is for the game to read: the build needs only default.xexp)
rem Steps:
rem   1. rexglue codegen: default.xex, patched by default.xexp as the game runs it -> project\generated (C++ for every
rem      function of the game)
rem   2. ninja + clang: the translated code -> build\sc5game.dll
rem   3. sc5.exe (ready made, without the game's code), sc5game.dll and the runtime's DLLs where SC5_GAME_EXE says
rem Every path in the compile is relative to this folder, so its name and place do not matter. Paths from outside are
rem only ever quoted or printed through a for variable, so a folder named with ( ) & ^ % ! is fine.
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0" || exit /b 1
set "TOOLS=..\..\toolchain\bin"
set "DISC=%SC5_GAME_DIR%"
if not defined DISC set "DISC=%~dp0disc"
set "EXE=%SC5_GAME_EXE%"
if not defined EXE set "EXE=%~dp0sc5.exe"
for %%F in ("%EXE%") do set "OUT=%%~dpF" & set "NAME=%%~nxF"
rem the game's DLL takes its hooks from the program named sc5.exe: no other name works
if /i not "%NAME%"=="sc5.exe" goto badname
if not exist "%DISC%\default.xex" goto nogame
if not exist "%DISC%\default.xexp" goto noupdate
if not exist host\sc5.exe goto missing
if not exist sdk\bin\rexglue.exe goto missing
if not exist project\title-update.txt goto missing
if not exist "%TOOLS%\clang.exe" goto missing
if not exist "%TOOLS%\lld-link.exe" goto missing
if not exist "%TOOLS%\ninja.exe" goto missing
rem a developer's settings for Microsoft's or clang's tools must not reach this build
set "LINK=" & set "_LINK_=" & set "CPATH=" & set "C_INCLUDE_PATH=" & set "CPLUS_INCLUDE_PATH=" & set "CCC_OVERRIDE_OPTIONS="
if not exist build mkdir build || exit /b 1

rem AVX2 PCs get the faster code, older ones the SSE4 build (clang knows this PC's CPU); both compute the same results
rem (the code builds without contracted multiply-adds). SC5_BUILD_MARCH: tests of the other one.
set "MARCH=x86-64-v2"
echo.| "%TOOLS%\clang.exe" -march=native -dM -E -x c - 2>nul | findstr /C:"__AVX2__" >nul && set "MARCH=x86-64-v3"
if defined SC5_BUILD_MARCH set "MARCH=%SC5_BUILD_MARCH%"

echo [1/3] Translating the game's code (codegen)...
rem codegen reads the executable where the manifest says (project\xex), with the update's patch beside it as the game
rem has them: the code is the updated game's
if not exist project\xex mkdir project\xex || exit /b 1
copy /y "%DISC%\default.xex" project\xex\default.xex >nul || goto cannotread
copy /y "%DISC%\default.xexp" project\xex\default.xexp >nul || goto cannotread
rem the update must be the one this release is built on (project\title-update.txt: "<default.xexp's SHA-1> <its
rem number>", from the launcher's sc5.json): the code of another one would not be the game the other player runs, and
rem the game would not start on it. The launcher checks it first; this is for a run by hand, and for an old update left
rem in place after SCV Online moved to a new one (title update 1 until 0.1.5, title update 3 since).
set "WANT=" & set "TUNUM=" & set "GOT="
for /f "usebackq tokens=1,2" %%A in ("project\title-update.txt") do set "WANT=%%A" & set "TUNUM=%%B"
for /f "skip=1 delims=" %%H in ('certutil -hashfile project\xex\default.xexp SHA1 2^>nul') do if not defined GOT set "GOT=%%H"
if not defined GOT goto cannothash
set "GOT=%GOT: =%"
if /i not "%GOT%"=="%WANT%" goto otherupdate
pushd project || exit /b 1
..\sdk\bin\rexglue.exe --log-level warn codegen sc5_manifest.toml
if errorlevel 1 goto codegenfailed
popd
rem the files build.ninja compiles: codegen keeps the release's split of the functions into files
rem (project\generated\default\codegen.partition.json), so they are the ones the release lists
fc /b project\sources.cmake project\generated\default\sources.cmake >nul 2>&1 || goto otherfiles

echo [2/3] Compiling (this takes a while the first time)...
>build\march.ninja echo march = %MARCH%
"%TOOLS%\ninja.exe" -f build.ninja
if errorlevel 1 goto compilefailed

echo [3/3] Installing...
if not exist "%OUT%" mkdir "%OUT%" || goto cannotinstall
copy /y host\sc5.exe "%OUT%sc5.exe" >nul || goto cannotinstall
copy /y build\sc5game.dll "%OUT%sc5game.dll" >nul || goto cannotinstall
copy /y sdk\bin\*.dll "%OUT%" >nul || goto cannotinstall
rem the release this build was made from: an update that changes it builds again
copy /y project\VERSION built.txt >nul
for %%P in ("%OUT%sc5.exe") do echo BUILD OK (%MARCH%): %%~P
exit /b 0

:badname
for %%P in ("%NAME%") do echo The game must be called sc5.exe, not %%~P: its code takes the game's hooks from sc5.exe.
exit /b 1

:nogame
for %%P in ("%DISC%") do echo The game's files are not there (no default.xex in %%~P): extract your disc first.
exit /b 1

:noupdate
for %%P in ("%DISC%") do echo The title update is not in place (no default.xexp beside default.xex in %%~P).
echo The game is built from the updated executable: put the update in place first.
exit /b 1

:missing
echo Part of SCV Online is missing from this folder (host\sc5.exe, sdk\bin\rexglue.exe, project\title-update.txt, or
echo clang, lld-link or ninja in ..\..\toolchain\bin): an antivirus may have taken it. Allow it there, or unpack the
echo release again.
exit /b 1

:cannothash
echo The title update could not be checked (Windows' certutil did not answer).
exit /b 1

:otherupdate
echo The title update in place is not title update %TUNUM% (its default.xexp has SHA-1 %GOT%).
echo SCV Online is built on title update %TUNUM% only: both players of a match must have the same update.
echo On Build game, choose title update %TUNUM% in step 2, then press CHECK AND BUILD.
exit /b 1

:cannotread
for %%P in ("%DISC%") do echo The game's executable could not be read from %%~P.
exit /b 1

:codegenfailed
popd
echo Translating the game's code failed.
exit /b 1

:otherfiles
echo Translating the game's code gave other files than this release's (project\sources.cmake): unpack the release
echo again over this folder, or check that the disc and the update are the ones the launcher asks for.
exit /b 1

:compilefailed
echo Compiling the game failed.
exit /b 1

:cannotinstall
for %%P in ("%OUT%") do echo The game could not be put in place in %%~P
echo Close the game if it is still running, and build again. If it is not, an antivirus may have taken sc5game.dll or
echo sc5.exe: allow it there (Windows Security, Virus and threat protection, Protection history) and build again.
exit /b 1
