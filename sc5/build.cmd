@echo off
rem Build the native Soulcalibur V executable. Usage: sc5\build.cmd [release|relwithdebinfo|debug]
call "%~dp0..\tools\devenv.cmd"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
cd /d "%~dp0"
if not exist "out\build\win-amd64-%CFG%\build.ninja" (
  cmake --preset win-amd64-%CFG% "-DCMAKE_PREFIX_PATH=%SC5_ROOT%\external\sdk" || exit /b 1
)
cmake --build out\build\win-amd64-%CFG% || exit /b 1
rem The SDK's DLLs are copied beside the game only when it is linked again: after an SDK change alone
rem (tools\build-sdk.cmd) the game would run with the old ones. Copy any that changed.
robocopy "%SC5_ROOT%\external\sdk\bin" "out\build\win-amd64-%CFG%" *.dll /XO /NJH /NJS /NFL /NDL /NP >nul
if errorlevel 8 exit /b 1
echo BUILD OK
