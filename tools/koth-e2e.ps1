<#
.SYNOPSIS
  King of the Hill end to end on one PC (docs/PROTOCOL.md 1.10.2): this checkout's server on a throwaway database and
  certificate and three launchers, A hosting a King of the Hill lobby, B joining it, C joining once both places are
  taken (it waits in line and watches the matches as a spectator), each with the stand-in game
  (launcher\tools\fake_game.py). A launcher readies whenever it is one of the two players and its game is free, and
  rates every match it watched once its game has shown the end (--rate). Each launcher starts one game, as PLAY does,
  and the test switches play their part through the asks of the game's menus (docs/PROTOCOL.md 3.8). Prints the games'
  status lines, the server's King of the Hill lines (king, next up, ratings, Respect Points), what each launcher did,
  and the database's Respect Points.

  The launchers run on Qt's offscreen platform (no window on the screen). Everything the run started is closed at its
  end.

.EXAMPLE
  powershell -File tools\koth-e2e.ps1 -Matches 3
#>
param(
    [int]$Seconds = 300,
    [int]$Matches = 3,           # lobby matches before the launchers stop readying (three show the line go round)
    [int]$MatchSeconds = 5,      # how long the stand-in's matches last (SC5_FAKE_SECONDS)
    [int]$Port = 8677,           # the server's TCP port (UDP Port + 1), the games' Port + 10 to + 13
    [string]$Exe = '',           # the game: default launcher\tools\fake_game.py (the stand-in)
    [string]$Launcher = '',      # default: build\launcher\SC5Launcher.exe
    [switch]$Snaps               # (the lobby page it took pictures of went with 0.1.8: nothing to take now)
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root  # python -m server ... from the checkout
if (-not $Exe) { $Exe = Join-Path $root 'launcher\tools\fake_game.py' }
if (-not $Launcher) { $Launcher = Join-Path $root 'build\launcher\SC5Launcher.exe' }
if (-not (Test-Path $Launcher)) { throw "no launcher at ${Launcher}: build it first (launcher\build.cmd)" }
$work = Join-Path $root ($(if ($Port -eq 8677) { 'build\koth-e2e' } else { "build\koth-e2e-$Port" }))
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force -Path "$work\a", "$work\b", "$work\c", "$work\d", "$work\server", "$work\snaps" | Out-Null
$py = (Get-Command python).Source
& $py -m server.makecert --out-dir "$work\server" | Out-Null
$fp = (& $py -m server.makecert --out-dir "$work\server" --show).Trim()
# the launchers nowhere on the screen: Qt's offscreen platform, which windeployqt leaves out
$plugin = Join-Path (Split-Path -Parent $Launcher) 'platforms\qoffscreen.dll'
if (-not (Test-Path $plugin)) {
    $qt = @("$root\external\qt\6.12.0\msvc2022_64", "$env:USERPROFILE\source\repos\mk3d-rollback\external\qt\6.12.0\msvc2022_64") |
        Where-Object { Test-Path "$_\plugins\platforms\qoffscreen.dll" } | Select-Object -First 1
    if (-not $qt) { throw 'qoffscreen.dll not found: put Qt in external\qt' }
    Copy-Item "$qt\plugins\platforms\qoffscreen.dll" $plugin
}
$i = 0
foreach ($who in 'a', 'b', 'c', 'd') {
    $ini = @"
[server]
address=127.0.0.1:$Port
fingerprint=$fp
auto_connect=true

[games]
sc5\exe=$($Exe -replace '\\', '/')
sc5\folder=$("$work\game" -replace '\\', '/')

[tools]
python=$($py -replace '\\', '/')

[online]
region=na
max_ping=0
delay=1
game_port=$($Port + 10 + $i)
pick_in_game=false
practice_while_waiting=false
search_again=false
hide_ip=false
match_sound=false
upnp=false

[account]
remember=false

[display]
mode=windowed
resolution=960x720

[dev]
udp_loopback=true
background=true
"@
    Set-Content -Path "$work\$who\sc5-launcher.ini" -Value $ini -Encoding ascii
    $i++
}
$server = Start-Process -FilePath $py -ArgumentList @('-m', 'server', '--host', '127.0.0.1', '--port', "$Port", '--udp-port', "$($Port + 1)", '--db', "`"$work\server\koth.sqlite`"", '--cert', "`"$work\server\server-cert.pem`"", '--key', "`"$work\server\server-key.pem`"") `
    -WorkingDirectory $root -PassThru -WindowStyle Hidden -RedirectStandardOutput "$work\server\out.txt" -RedirectStandardError "$work\server\err.txt"
Start-Sleep -Seconds 2
$vars = @('SC5_DEV_PASSWORD', 'SC5_FAKE_SECONDS', 'QT_QPA_PLATFORM', 'QT_QPA_FONTDIR')
$env:SC5_DEV_PASSWORD = 'koth-e2e-pass-123'  # throwaway accounts on a throwaway server
$env:SC5_FAKE_SECONDS = "$MatchSeconds"
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QPA_FONTDIR = "$env:WINDIR\Fonts"  # the offscreen platform finds no fonts by itself (the pictures' text)
$stamp = Get-Date -Format 'HHmmss'
$common = @('--lobby-name', "koth$stamp", '--lobby-matches', "$Matches", '--mode', 'versus')
function Start-Launcher($who, $user, $extra) {
    $p = Start-Process -FilePath $Launcher -ArgumentList (@('--config', "`"$work\$who\sc5-launcher.ini`"", '--user', $user, '--register') + $extra) -PassThru
    Start-Sleep -Milliseconds 500
    $p
}
$users = @{ a = "HillA$stamp"; b = "HillB$stamp"; c = "HillC$stamp"; d = "HillD$stamp" }
$la = Start-Launcher 'a' $users.a (@('--lobby', 'host', '--koth', '--character', 'Siegfried', '--rate', '5') + $common)
Start-Sleep -Seconds 3
$lb = Start-Launcher 'b' $users.b (@('--lobby', 'join', '--character', 'Mitsurugi', '--rate', '7') + $common)
Start-Sleep -Seconds 4
$lc = Start-Launcher 'c' $users.c (@('--lobby', 'join', '--character', 'Ivy', '--rate', '9') + $common)
"server pid $($server.Id), launchers A $($la.Id) B $($lb.Id) C $($lc.Id); accounts $($users.a) $($users.b) $($users.c)"
$ls = @($la, $lb, $lc)
$t0 = Get-Date
$seen = @{}
$doneAt = $null
$finished = $false
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    foreach ($s in Get-ChildItem -Path "$work\a\sessions", "$work\b\sessions", "$work\c\sessions" -Directory -ErrorAction SilentlyContinue) {
        $f = Join-Path $s.FullName 'status.txt'
        if (-not (Test-Path $f)) { continue }
        $line = (Get-Content $f -TotalCount 1 -ErrorAction SilentlyContinue)
        $key = "$($s.Parent.Parent.Name)/$($s.Name)"
        if ($line -and $seen[$key] -ne $line) {
            $seen[$key] = $line
            "{0,6:N1}s {1}: {2}" -f ((Get-Date) - $t0).TotalSeconds, $key, $line
        }
    }
    $serverLog = @(Get-Content "$work\server\err.txt", "$work\server\out.txt" -ErrorAction SilentlyContinue)
    foreach ($ln in $serverLog | Select-String -Pattern 'lobby \w+: .*(king|rates|rated|did not ready|Next up|left)|RESULT|DISPUTE|ABANDONED|NO_CONTEST|DESYNC|cancelled|Traceback') {
        if (-not $seen[$ln.Line]) { $seen[$ln.Line] = 1; "{0,6:N1}s server: {1}" -f ((Get-Date) - $t0).TotalSeconds, ($ln.Line -replace '^\S+ \S+ \S+\s+', '') }
    }
    $results = @($serverLog | Select-String -Pattern 'RESULT match=').Count
    $closed = @($serverLog | Select-String -Pattern 'rated, ').Count
    if ($results -ge $Matches -and -not $doneAt) { $doneAt = Get-Date }
    # the last match's rating closes once its spectator rated (after their game showed its end), 60 s on at most
    if ($doneAt -and ($closed -ge $Matches -or ((Get-Date) - $doneAt).TotalSeconds -gt 60)) { $finished = $closed -ge $Matches; Start-Sleep -Seconds 4; break }
    if (@($ls | Where-Object { $_.HasExited }).Count) { "a launcher exited"; break }
    Start-Sleep -Milliseconds 500
}

"--- launcher logs (cli, rating, watch and match lines)"
foreach ($s in 'a', 'b', 'c') {
    Get-Content "$work\$s\sc5-launcher.log" -ErrorAction SilentlyContinue | Select-String -Pattern 'cli:|match: (Lobby match|Watching|Done watching|Match over|Starting|The players finished|Match cancelled|Cannot)|error' | ForEach-Object { "[$s] $($_.Line)" }
}
"--- game sessions: match starts and watching"
foreach ($s in 'a', 'b', 'c') {
    Get-ChildItem -Path "$work\$s\sessions" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $log = Join-Path $_.FullName 'game.log'
        if (Test-Path $log) { Select-String -Path $log -Pattern 'link: start' | Select-Object -First 30 | ForEach-Object { "[$s] $($_.Line)" } }
    }
}
"--- server: lobby and result lines"
Get-Content "$work\server\err.txt", "$work\server\out.txt" -ErrorAction SilentlyContinue | Select-String -Pattern 'lobby |RESULT| go: |feed' | ForEach-Object { $_.Line -replace '^\S+ \S+ \S+\s+', '' }
# close only what this run started: the launchers and their games, then the server
$all = @($la, $lb, $lc)
$games = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.ParentProcessId -in @($all | ForEach-Object { $_.Id }) } | ForEach-Object { $_.ProcessId })
foreach ($p in $all) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 3
foreach ($id in @($all | ForEach-Object { $_.Id }) + $games) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
foreach ($k in $vars) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2  # processes just stopped are still there for a moment
"--- database"
& $py -I -c "import sqlite3,sys; c=sqlite3.connect(sys.argv[1]); print('respect', c.execute('SELECT a.name, r.points, r.ratings FROM respect r JOIN accounts a ON a.id=r.account_id ORDER BY a.name').fetchall()); print('ratings', c.execute('SELECT r.match_id, a.name, w.name, r.score FROM ratings r JOIN accounts a ON a.id=r.rater JOIN accounts w ON w.id=r.winner ORDER BY r.rated').fetchall()); print('matches', c.execute('SELECT id, status, winner, p1_rounds, p2_rounds FROM matches ORDER BY created').fetchall())" "$work\server\koth.sqlite"
"still running from this run: $(@(Get-Process -Id (@($all | ForEach-Object { $_.Id }) + $games + $server.Id) -ErrorAction SilentlyContinue).Count)"
if ($finished) { "PASS: King of the Hill, $Matches matches rated" } else { "FAIL: not finished within $Seconds s" }
