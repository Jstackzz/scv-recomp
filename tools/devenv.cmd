@echo off
rem Developer environment: MSVC libraries and Windows SDK from VS 2026 Build Tools, clang from SC5_LLVM
rem (default: mk9-rollback's external\llvm until this repository has its own), CMake and Ninja from VS.
rem Usage: call tools\devenv.cmd
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "SC5_ROOT=%~dp0.."
if "%SC5_LLVM%"=="" if exist "%SC5_ROOT%\external\llvm\bin\clang.exe" set "SC5_LLVM=%SC5_ROOT%\external\llvm"
if "%SC5_LLVM%"=="" set "SC5_LLVM=%USERPROFILE%\source\repos\mk9-rollback\external\llvm"
set "PATH=%SC5_LLVM%\bin;%SC5_ROOT%\external\sdk\bin;C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
