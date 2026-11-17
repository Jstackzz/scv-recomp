# Starts a built game (a package's games\sc5\sc5.exe after build-game.cmd) as the launcher starts it for a match,
# windowed, with a PATH of Windows only, and waits until it is at the main menu: `ready` in status.txt (docs\PROTOCOL.md
# 3.4). Then it ends the game. Everything the game writes goes into -Work (saves, shader cache, log), which must be a
# folder of the test's own; -Disc and -Update are only read (the repository's game\disc and game\update will do).
# Usage: tools\player\test-boot.ps1 -Exe <package>\games\sc5\sc5.exe -Work <folder> [-Disc <folder>] [-Update <folder>]
#                                   [-Seconds 300]
param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string]$Work, [string]$Disc = "", [string]$Update = "",
      [int]$Seconds = 300)
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$game = Split-Path $Exe -Parent
if (-not $Disc) { $Disc = Join-Path $game "disc" }
if (-not $Update) { $Update = Join-Path $game "update" }
New-Item -ItemType Directory -Force (Join-Path $Work "user"), (Join-Path $Work "cache"), (Join-Path $Work "dlc") | Out-Null
$Work = (Resolve-Path -LiteralPath $Work).Path
$status = Join-Path $Work "status.txt"
Remove-Item -LiteralPath $status, (Join-Path $Work "cmd.txt") -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = "--fullscreen=false --window_width=960 --window_height=540"
$psi.WorkingDirectory = $game  # as the launcher: the game's own folder
$psi.UseShellExecute = $false
$keep = @{}
foreach ($n in "SystemRoot", "windir", "SystemDrive", "ComSpec", "PATHEXT", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE", "PROCESSOR_IDENTIFIER", "OS", "TEMP", "TMP", "USERPROFILE", "LOCALAPPDATA", "APPDATA", "ProgramData", "ProgramFiles", "ProgramW6432", "CommonProgramFiles", "HOMEDRIVE", "HOMEPATH", "USERNAME", "COMPUTERNAME") {
    $v = [Environment]::GetEnvironmentVariable($n)
    if ($v) { $keep[$n] = $v }
}
$psi.EnvironmentVariables.Clear()
foreach ($k in $keep.Keys) { $psi.EnvironmentVariables[$k] = $keep[$k] }
$psi.EnvironmentVariables["PATH"] = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
$vars = @{
    SC5_NET = "1"; SC5_GAME = "sc5"; SC5_INSTANCE = "1"; SC5_SESSION_DIR = $Work
    SC5_CMD = (Join-Path $Work "cmd.txt"); SC5_STATUS = $status; SC5_LOG = (Join-Path $Work "game.log")
    SC5_GAME_DIR = $Disc; SC5_UPDATE_DIR = $Update; SC5_DLC_DIR = (Join-Path $Work "dlc")
    SC5_USER_DIR = (Join-Path $Work "user"); SC5_CACHE_DIR = (Join-Path $Work "cache")
}
foreach ($k in $vars.Keys) { $psi.EnvironmentVariables[$k] = $vars[$k] }

function First-Line([string]$path) {
    try {
        $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete)
        try { return (New-Object IO.StreamReader($fs)).ReadLine() } finally { $fs.Dispose() }
    } catch { return $null }
}

$sw = [Diagnostics.Stopwatch]::StartNew()
$p = [Diagnostics.Process]::Start($psi)
"started $Exe (pid $($p.Id))"
$last = $null
$result = "timeout"
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    if ($p.HasExited) { $result = "exited"; break }
    $line = First-Line $status
    if ($line -and $line -ne $last) {
        "{0,5:N1} s  {1}" -f $sw.Elapsed.TotalSeconds, $line
        $last = $line
        if ($line -eq "ready") { $result = "ready"; break }
        if ($line.StartsWith("error")) { $result = "error"; break }
    }
    Start-Sleep -Milliseconds 250
}
$t = [int]$sw.Elapsed.TotalSeconds
if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit(10000) | Out-Null }
switch ($result) {
    "ready" { "READY after $t s (the game was ended)" }
    "exited" { "the game exited after $t s with code $($p.ExitCode)" }
    default { "no ready after $t s ($result, last status: $last; the game was ended)" }
}
$log = Join-Path $Work "game.log"
if ($result -ne "ready" -and (Test-Path -LiteralPath $log)) { "--- the end of game.log:"; Get-Content -LiteralPath $log -Tail 25 }
if ($result -ne "ready") { exit 1 }
