# Runs a package's build-game.cmd as the launcher does (BuildPage::compileGame) on a PC with nothing installed: no
# compiler environment, a PATH of Windows only, SC5_GAME_DIR, SC5_UPDATE_DIR and SC5_GAME_EXE as the launcher sets them.
# Usage: tools\player\test-clean-build.ps1 -Game <package>\games\sc5 [-Disc <folder>] [-Update <folder>] [-Exe <file>]
#                                          [-March x86-64-v2] [-Log <file>]
# -Disc defaults to <game>\disc (the launcher's), and needs only default.xex and default.xexp there: the repository's
# game\disc will do, read only. -March x86-64-v2 is what a CPU without AVX2 builds. tools\player\check_package.py
# checks what the result needs.
param([Parameter(Mandatory)][string]$Game, [string]$Disc = "", [string]$Update = "", [string]$Exe = "",
      [string]$March = "", [string]$Log = "")
$Game = (Resolve-Path -LiteralPath $Game).Path
$script = Join-Path $Game "build-game.cmd"
if (-not (Test-Path -LiteralPath $script)) { throw "no $script" }
if (-not $Disc) { $Disc = Join-Path $Game "disc" }
if (-not $Update) { $Update = Join-Path $Game "update" }
if (-not $Exe) { $Exe = Join-Path $Game "sc5.exe" }
if (-not $Log) { $Log = Join-Path (Split-Path (Split-Path (Split-Path $Game))) "build-out.txt" }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$env:SystemRoot\System32\cmd.exe"
$psi.Arguments = "/d /s /c `"`"$script`" `""  # how the launcher runs it
$psi.WorkingDirectory = $Game
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$keep = @{}
foreach ($n in "SystemRoot", "windir", "SystemDrive", "ComSpec", "PATHEXT", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER", "OS", "TEMP", "TMP", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "ProgramData", "ProgramFiles", "ProgramW6432", "CommonProgramFiles", "HOMEDRIVE", "HOMEPATH", "USERNAME", "COMPUTERNAME") {
    $v = [Environment]::GetEnvironmentVariable($n)
    if ($v) { $keep[$n] = $v }
}
$psi.EnvironmentVariables.Clear()
foreach ($k in $keep.Keys) { $psi.EnvironmentVariables[$k] = $keep[$k] }
$psi.EnvironmentVariables["PATH"] = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
$psi.EnvironmentVariables["SC5_GAME_DIR"] = $Disc
$psi.EnvironmentVariables["SC5_UPDATE_DIR"] = $Update
$psi.EnvironmentVariables["SC5_GAME_EXE"] = $Exe
if ($March) { $psi.EnvironmentVariables["SC5_BUILD_MARCH"] = $March }
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = [Diagnostics.Process]::Start($psi)
$errTask = $p.StandardError.ReadToEndAsync()
$out = $p.StandardOutput.ReadToEnd()
$p.WaitForExit()
$sw.Stop()
[IO.File]::WriteAllText($Log, $out + "`n--- stderr ---`n" + $errTask.Result)
"exit code $($p.ExitCode), $([int]$sw.Elapsed.TotalSeconds) s, log $Log"
($out -split "`n" | Select-Object -Last 6) -join "`n"
