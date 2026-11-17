@echo off
rem Recompile the game's PowerPC code to C++ (sc5\generated\default) with the SDK's code generator.
rem Needs game\disc\default.xex with the title update's default.xexp beside it (docs\TARGET.md).
rem Usage: sc5\codegen.cmd   then sc5\build.cmd release
setlocal
call "%~dp0..\tools\devenv.cmd"
cd /d "%~dp0" || exit /b 1
rexglue --log-level warn codegen sc5_manifest.toml || exit /b 1
echo CODEGEN OK
