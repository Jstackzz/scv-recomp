# Assembles the player package in dist\SC5-Online: no game data and no game code, each player builds their own copy
# from their own disc (tools\player\build-game.cmd). Nothing else has to be installed on the player's PC:
# sc5\player\README.md says how.
#   SC5Launcher.exe + Qt          the launcher (build\launcher, after launcher\build.cmd); left out with -NoLauncher
#   Microsoft's C++ runtime DLLs  beside the launcher and in games\sc5\sdk\bin, for the PCs without them
#   toolchain\                    clang, lld-link, ninja and clang's own headers
#   games\sc5\build-game.cmd      codegen + compile on the player's PC; build.ninja is the compile
#   games\sc5\host\sc5.exe        everything but the game's code (tools\build-player-host.cmd -> dist\host)
#   games\sc5\project\            what codegen reads: the manifest (pointing at project\xex, where build-game.cmd puts
#                                 the player's default.xex and default.xexp), the config, the game's VERSION, and this
#                                 release's split of the functions into files (generated\default\codegen.partition.json,
#                                 so every PC's codegen writes the files build.ninja names; sources.cmake, their list)
#   games\sc5\player\             the headers and import libraries the game's code compiles and links with
#   games\sc5\sdk\                the prebuilt ReXGlue runtime and codegen tool (external\sdk)
#   docs\                         GAME.md, LAUNCHER.md
#   LICENSES\                     this project's, the ReXGlue SDK's (its runtime and codegen tool ship here), SDL3's
#                                 and, with the launcher, Qt's; LLVM's and Ninja's are in toolchain\
#   defaults.ini                  the server the launcher uses until a player picks another: dist\defaults.ini (not in
#                                 the repository: it names the machine), [server] address= and fingerprint=
# The folder is named SC5-Online with VERSION.txt ("SC5-Online <launcher version>, packaged <date>"): what the
# launcher's updater checks in a release zip (launcher/src/Updater.cpp).
# Usage: . tools\package.ps1; New-PlayerPackage [-Out dist\SC5-Online] [-HostBuild dist\host] [-NoLauncher]
function New-PlayerPackage {
    param([string]$Out = "", [string]$HostBuild = "", [switch]$NoLauncher)
    $ErrorActionPreference = "Stop"
    $root = Split-Path $PSScriptRoot -Parent
    if (-not $root) { $root = (Get-Location).Path }  # pasted rather than dot-sourced: run it from the repository
    if (-not $Out) { $Out = Join-Path $root "dist\SC5-Online" }
    if (-not $HostBuild) { $HostBuild = Join-Path $root "dist\host" }
    $launcherVersion = [regex]::Match((Get-Content -Raw (Join-Path $root "launcher\CMakeLists.txt")), 'project\(sc5_launcher VERSION ([0-9.]+)').Groups[1].Value
    if (-not $launcherVersion) { throw "no launcher version in launcher\CMakeLists.txt" }
    $sdk = Join-Path $root "external\sdk"
    if (-not (Test-Path (Join-Path $sdk "bin\rexglue.exe"))) { throw "no SDK in ${sdk}: tools\build-sdk.cmd" }
    $llvm = $env:SC5_LLVM
    if (-not $llvm -and (Test-Path (Join-Path $root "external\llvm\bin\clang.exe"))) { $llvm = Join-Path $root "external\llvm" }
    if (-not $llvm) { $llvm = Join-Path $env:USERPROFILE "source\repos\mk9-rollback\external\llvm" }  # as tools\devenv.cmd
    if (-not (Test-Path (Join-Path $llvm "bin\clang.exe"))) { throw "no clang in $llvm (SC5_LLVM)" }
    $clangVer = @(Get-ChildItem (Join-Path $llvm "lib\clang") -Directory)
    if ($clangVer.Count -ne 1) { throw "not one folder of clang's headers in $llvm\lib\clang" }
    $clangVer = $clangVer[0].Name
    $gen = Join-Path $root "sc5\generated\default"
    foreach ($f in "sources.cmake", "codegen.partition.json") {
        if (-not (Test-Path (Join-Path $gen $f))) { throw "no $f in ${gen}: sc5\codegen.cmd" }
    }

    # sc5.exe without the game's code, and what the game's code compiles and links with on the player's PC
    foreach ($f in "sc5.exe", "sc5.lib", "player\game_link.rsp") {
        if (-not (Test-Path (Join-Path $HostBuild $f))) { throw "no $f in ${HostBuild}: run tools\build-player-host.cmd first" }
    }
    $hostExe = Get-Item (Join-Path $HostBuild "sc5.exe")
    $newer = @(Get-ChildItem (Join-Path $root "sc5\config") -File | Where-Object { $_.LastWriteTime -gt $hostExe.LastWriteTime })
    if ($newer.Count) { throw "the codegen config changed after $($hostExe.FullName) was built ($($newer.Name -join ', ')): tools\build-player-host.cmd again" }

    New-Item -ItemType Directory -Force $Out | Out-Null
    $game = Join-Path $Out "games\sc5"
    $proj = Join-Path $game "project"
    New-Item -ItemType Directory -Force (Join-Path $proj "generated\default") | Out-Null

    # the game's project: what codegen reads. The manifest reads the executable from project\xex, where build-game.cmd
    # copies the player's default.xex and default.xexp from wherever the launcher extracted them (SC5_GAME_DIR)
    Copy-Item -Force (Join-Path $root "sc5\VERSION") $proj  # the game's release, not the launcher's (sc5\CMakeLists.txt)
    robocopy (Join-Path $root "sc5\config") (Join-Path $proj "config") /MIR /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for the config" }
    $manifest = Get-Content -Raw (Join-Path $root "sc5\sc5_manifest.toml")
    $manifest = $manifest -replace '(?m)^game_root\s*=\s*"[^"]*"', 'game_root = "xex"'
    $manifest = $manifest -replace '(?m)^(\[entrypoint\][\s\S]*?^)file_path\s*=\s*"[^"]*"', '$1file_path = "xex/default.xex"'
    if ($manifest -notmatch '(?m)^game_root = "xex"' -or $manifest -notmatch '(?m)^file_path = "xex/default.xex"') {
        throw "sc5\sc5_manifest.toml has no game_root or entrypoint file_path to point at project\xex"
    }
    [System.IO.File]::WriteAllText((Join-Path $proj "sc5_manifest.toml"), $manifest)
    Copy-Item -Force (Join-Path $gen "codegen.partition.json") (Join-Path $proj "generated\default")
    Copy-Item -Force (Join-Path $gen "sources.cmake") $proj
    # (with Windows line ends whatever the checkout has: cmd.exe can miss a label in a script without them)
    $script = (Get-Content -Raw (Join-Path $root "tools\player\build-game.cmd")) -replace "`r?`n", "`r`n"
    [System.IO.File]::WriteAllText((Join-Path $game "build-game.cmd"), $script, [System.Text.Encoding]::ASCII)

    $hostDir = Join-Path $game "host"
    New-Item -ItemType Directory -Force $hostDir | Out-Null
    Copy-Item -Force $hostExe.FullName $hostDir
    $player = Join-Path $game "player"
    robocopy (Join-Path $root "sc5\player\include") (Join-Path $player "include") /MIR /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for the player headers" }
    Copy-Item -Force (Join-Path $root "sc5\player\game_stub.cpp") $player
    Copy-Item -Force (Join-Path $HostBuild "player\game_link.rsp") $player
    $libs = Join-Path $player "lib"
    New-Item -ItemType Directory -Force $libs | Out-Null
    Copy-Item -Force (Join-Path $HostBuild "sc5.lib") $libs
    foreach ($d in "msvcp140", "vcruntime140", "ucrtbase") {
        & (Join-Path $llvm "bin\llvm-lib.exe") /nologo /machine:x64 "/def:$(Join-Path $root "sc5\player\$d.def")" "/out:$(Join-Path $libs "$d.lib")"
        if ($LASTEXITCODE -ne 0) { throw "llvm-lib failed for $d" }
    }
    Remove-Item -Force (Join-Path $libs "*.exp") -ErrorAction SilentlyContinue
    python (Join-Path $root "tools\player\make_build_ninja.py") --sources (Join-Path $gen "sources.cmake") --clang-version $clangVer --out (Join-Path $game "build.ninja") | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "make_build_ninja failed" }

    # the prebuilt runtime and codegen tool (not TracyClient.dll: nothing in this SDK build loads it), and the SDK
    # headers the game's code compiles with (player\include stands in for the rest)
    $sdkTo = Join-Path $game "sdk"
    New-Item -ItemType Directory -Force (Join-Path $sdkTo "bin"), (Join-Path $sdkTo "lib") | Out-Null
    foreach ($f in "rexglue.exe", "rexruntime.dll", "rexgpu-xenos.dll") { Copy-Item -Force (Join-Path $sdk "bin\$f") (Join-Path $sdkTo "bin") }
    Copy-Item -Force (Join-Path $sdk "lib\rexruntime.lib") (Join-Path $sdkTo "lib")
    robocopy (Join-Path $sdk "include\simde") (Join-Path $sdkTo "include\simde") /MIR /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for simde" }
    foreach ($h in "rex\platform.h", "rex\image_info.h", "rex\platform\fpscr.h", "rex\ppc\context.h", "rex\ppc\func.h", "rex\ppc\intrinsics.h") {
        $to = Join-Path $sdkTo "include\$h"
        New-Item -ItemType Directory -Force (Split-Path $to -Parent) | Out-Null
        Copy-Item -Force (Join-Path $sdk "include\$h") $to
    }

    # the game and the codegen tool keep paths in narrow strings: as UTF-8 programs they work from any folder, a Windows
    # account's name with an accent or another script in it too
    $mt = @(Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\mt.exe" | Sort-Object FullName)
    if ($mt.Count -eq 0) { throw "no mt.exe in the Windows SDK" }
    foreach ($exe in (Join-Path $hostDir "sc5.exe"), (Join-Path $sdkTo "bin\rexglue.exe")) {
        & $mt[-1].FullName -nologo -manifest (Join-Path $root "sc5\player\utf8.manifest") "-updateresource:$exe;#1"
        if ($LASTEXITCODE -ne 0) { throw "mt failed for $exe" }
    }

    # Microsoft's C++ runtime (redistributable with an application): the launcher, the codegen tool and the game need it,
    # and a PC that has never had it, or has an older one, would stop at "VCRUNTIME140.dll was not found"
    $redist = @(Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName)
    if ($redist.Count -eq 0) { throw "no Microsoft.VC*.CRT folder under Visual Studio's VC\Redist" }
    $crt = $redist[-1].FullName
    foreach ($d in "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll") {
        Copy-Item -Force (Join-Path $crt $d) (Join-Path $sdkTo "bin")
        if (-not $NoLauncher) { Copy-Item -Force (Join-Path $crt $d) $Out }
    }

    # clang, lld-link and clang's headers: what build.ninja needs of LLVM. Ninja runs it (the copy in Visual Studio's CMake:
    # Apache-2.0, its licence beside it)
    $tc = Join-Path $Out "toolchain"
    New-Item -ItemType Directory -Force (Join-Path $tc "bin") | Out-Null
    Copy-Item -Force (Join-Path $llvm "bin\clang.exe") (Join-Path $tc "bin\clang.exe")
    Copy-Item -Force (Join-Path $llvm "bin\lld-link.exe") (Join-Path $tc "bin\lld-link.exe")
    $ninja = @(Get-ChildItem "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" | Sort-Object FullName)
    if ($ninja.Count -eq 0) { throw "no ninja.exe in Visual Studio's CMake" }
    Copy-Item -Force $ninja[-1].FullName (Join-Path $tc "bin\ninja.exe")
    robocopy (Join-Path $llvm "lib\clang\$clangVer\include") (Join-Path $tc "lib\clang\$clangVer\include") /MIR /NFL /NDL /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for clang's headers" }
    if (Test-Path (Join-Path $llvm "LICENSE.TXT")) { Copy-Item -Force (Join-Path $llvm "LICENSE.TXT") (Join-Path $tc "LICENSE.TXT") }
    Copy-Item -Force (Join-Path $root "tools\player\NINJA-LICENSE.txt") (Join-Path $tc "NINJA-LICENSE.txt")

    # the licenses of what the package carries (the ReXGlue SDK's BSD licence asks for its notice with its binaries)
    $lic = Join-Path $Out "LICENSES"
    New-Item -ItemType Directory -Force $lic | Out-Null
    Copy-Item -Force (Join-Path $root "LICENSE") (Join-Path $lic "SC5-Online-LICENSE.txt")
    $third = [ordered]@{
        "ReXGlue-SDK-LICENSE.txt" = (Join-Path $root "external\rexglue-sdk\LICENSE")
        "SDL3-LICENSE.txt"        = (Join-Path $sdk "licenses\SDL3\LICENSE.txt")
    }
    foreach ($name in $third.Keys) {
        if (Test-Path $third[$name]) { Copy-Item -Force $third[$name] (Join-Path $lic $name) }
        else { Write-Host "no $($third[$name]): $name left out of the package" }
    }
    if (-not $NoLauncher) { Copy-Item -Force (Join-Path $root "launcher\licenses\*") $lic }

    if (-not $NoLauncher) {
        # the server: dist\defaults.ini, kept out of the repository
        $defaults = Join-Path $root "dist\defaults.ini"
        if (Test-Path $defaults) {
            Copy-Item -Force $defaults (Join-Path $Out "defaults.ini")
        } else {
            Write-Host "no dist\defaults.ini: the package's launcher looks for a server on the player's own PC (127.0.0.1:7777)"
        }
        # the launcher (windeployqt already put Qt beside it); the build's own files stay behind (moc output, the static
        # SDL3's build folder, the compile commands), and windeployqt's installer of the runtime the package has as DLLs
        $lb = Join-Path $root "build\launcher"
        if (-not (Test-Path (Join-Path $lb "SC5Launcher.exe"))) { throw "no launcher in $lb (launcher\build.cmd), or -NoLauncher" }
        robocopy $lb $Out /E /XD CMakeFiles _deps .qt SC5Launcher_autogen sdl3 /XF *.ninja* CMakeCache.txt cmake_install.cmake compile_commands.json *.obj *.pdb *.ilk *.exp *.lib *.rc vc_redist.x64.exe /NFL /NDL /NJH /NJS | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed for the launcher" }
        New-Item -ItemType Directory -Force (Join-Path $Out "docs") | Out-Null
        foreach ($d in "GAME.md", "LAUNCHER.md") {
            $p = Join-Path $root "docs\$d"
            if (Test-Path $p) { Copy-Item -Force $p (Join-Path $Out "docs") }
        }
        $packaged = Get-Date -Format 'yyyy-MM-dd HH:mm'
        [System.IO.File]::WriteAllText((Join-Path $Out "VERSION.txt"), "SC5-Online $launcherVersion, packaged $packaged`r`n")
        # the release's update.json names it, so a test build of the same version is offered the release (PROTOCOL.md 1.3)
        "update.json for this package: `"version`": `"$launcherVersion`", `"build`": `"$packaged`""
    }
    $size = (Get-ChildItem $Out -Recurse -File | Measure-Object -Property Length -Sum).Sum
    "package in $Out ({0:N0} MB)" -f ($size / 1MB)
}
