@echo off
rem Build this project's copy of the ReXGlue SDK (external\rexglue-sdk: v0.10.0 with
rem patches\rexglue-sdk-v0.10.0.patch applied, see docs\SDK.md) and install it into external\sdk, where
rem sc5\build.cmd finds it. Usage:
rem   tools\build-sdk.cmd            build everything (Release) and install it into external\sdk
rem   tools\build-sdk.cmd rexglue    build one target only, nothing installed
call "%~dp0devenv.cmd"
cd /d "%SC5_ROOT%\external\rexglue-sdk" || exit /b 1
if not exist out\build\win-amd64\build.ninja (
  cmake --preset win-amd64 || exit /b 1
)
if not "%~1"=="" (
  cmake --build out\build\win-amd64 --config Release --target %~1 || exit /b 1
  echo BUILD OK
  exit /b 0
)
cmake --build out\build\win-amd64 --config Release || exit /b 1
rem a private copy: not registered in CMake's user package registry, where other projects would find it
set "REXGLUE_NO_PACKAGE_REGISTRY=1"
cmake --install out\build\win-amd64 --config Release --prefix "%SC5_ROOT%\external\sdk" || exit /b 1
echo BUILD OK
