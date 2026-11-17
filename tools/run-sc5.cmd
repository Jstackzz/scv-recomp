@echo off
rem Start the native Soulcalibur V that sc5\build.cmd built, with this repository's game files and saves
rem (game\disc, game\update, game\user). Extra arguments go to the game, for example a window instead of the full
rem screen: tools\run-sc5.cmd --fullscreen=false --window_width=1280 --window_height=720
setlocal
set "ROOT=%~dp0.."
set "SC5_GAME_DIR=%ROOT%\game\disc"
set "SC5_UPDATE_DIR=%ROOT%\game\update"
set "SC5_USER_DIR=%ROOT%\game\user"
if not exist "%ROOT%\sc5\out\build\win-amd64-release\sc5.exe" (
  echo Build the game first: sc5\build.cmd release
  exit /b 1
)
start "" "%ROOT%\sc5\out\build\win-amd64-release\sc5.exe" %*
