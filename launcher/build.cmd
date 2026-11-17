@echo off
rem Build the SC5 Online launcher (Release) into build\launcher and copy the Qt runtime next to it.
rem Nothing is downloaded. Qt comes from QT_DIR (default: external\qt\6.12.0\msvc2022_64, else MK3D Online's copy),
rem SDL3's sources from SDL3_DIR (default: external\sdl3, else the copy in MK3D Online's game build), MSVC, CMake and
rem Ninja from the VS 2026 Build Tools; BUILD_DIR picks another build folder. See docs\LAUNCHER.md.
setlocal
set "ROOT=%~dp0.."
set "MK3D=%USERPROFILE%\source\repos\mk3d-rollback"
if "%QT_DIR%"=="" if exist "%ROOT%\external\qt\6.12.0\msvc2022_64\bin\windeployqt.exe" set "QT_DIR=%ROOT%\external\qt\6.12.0\msvc2022_64"
if "%QT_DIR%"=="" set "QT_DIR=%MK3D%\external\qt\6.12.0\msvc2022_64"
if "%SDL3_DIR%"=="" if exist "%ROOT%\external\sdl3\CMakeLists.txt" set "SDL3_DIR=%ROOT%\external\sdl3"
if "%SDL3_DIR%"=="" set "SDL3_DIR=%MK3D%\recomp\mkd\out\build\release\_deps\sdl3-src"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%ROOT%\build\launcher"
if not exist "%QT_DIR%\bin\windeployqt.exe" goto noqt
if not exist "%SDL3_DIR%\CMakeLists.txt" goto nosdl
set "VS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" goto novs
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S "%ROOT%\launcher" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl "-DCMAKE_PREFIX_PATH=%QT_DIR%" "-DSC5_SDL3_DIR=%SDL3_DIR%"
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%BUILD_DIR%\SC5Launcher.exe"
if errorlevel 1 exit /b 1
echo.
echo Built %BUILD_DIR%\SC5Launcher.exe
exit /b 0

:noqt
echo Qt not found in "%QT_DIR%". Set QT_DIR, see docs\LAUNCHER.md.
exit /b 1

:nosdl
echo SDL3 sources not found in "%SDL3_DIR%". Set SDL3_DIR, see docs\LAUNCHER.md.
exit /b 1

:novs
echo The VS 2026 Build Tools are not in "%VS%".
exit /b 1
