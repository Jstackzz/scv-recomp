<#
.SYNOPSIS
  SC5 Online end to end on one PC: this checkout's server on a throwaway database and certificate, two launchers
  (separate settings, game ports Port+10/+11) that sign in, find each other and play, and the games they start: the
  stand-in game (launcher\tools\fake_game.py) unless -Exe names another. Prints every status line of the games, what
  the launchers did (match found, accept, rematch, set), the server's match lines and the accounts' records.

  -Queue casual (default): a Casual search, MATCH FOUND accepted after -AcceptB seconds, the match, its result, then
  -Rematches rematches. -Queue ranked: a ranked set (first to 3), played on by itself until one player wins it, the Elo
  and the rank titles. -Queue lobby: A hosts a lobby, B joins it, both ready and -Matches matches play; -Watch adds a
  third launcher, C, that watches the lobby as a spectator (its game plays each match from the players' feeds).
  -Mode sparring with -Queue lobby: Online Practice. -Queue asks: no test switches play the players' part; each
  launcher only starts its game (--play, as PLAY), and the stand-in games drive everything from a script of asks
  (SC5_FAKE_ASKS, docs/PROTOCOL.md 3.8), reading the launchers' ui.json: a casual match, refused asks, leaving the
  series, then a lobby that A creates and B joins from the list, its match, the leaderboard, leaving the lobby.

  Each launcher starts one game for everything, as PLAY does (SCV 0.1.8): every match of the run plays in it.

  -Exe runs a real sc5.exe instead of the stand-in, with -Game naming the game folder whose disc, update and dlc it
  plays: they are linked into the run's own game folder (junctions, removed each on its own before anything deletes
  it), and the games' shader cache and saves stay in the run's folder.

  The launchers run on Qt's offscreen platform (no window on the screen) and the stand-in games have none, unless
  -Visible. Everything the run started is closed at its end; its folder (build\online-e2e) stays for a look and is
  replaced by the next run.

.EXAMPLE
  powershell -File tools\online-e2e.ps1 -Queue casual -Rematches 1
  powershell -File tools\online-e2e.ps1 -Queue ranked
  powershell -File tools\online-e2e.ps1 -Queue lobby -Matches 2 -Watch
  powershell -File tools\online-e2e.ps1 -Queue ranked -Draw -Matches 2
  powershell -File tools\online-e2e.ps1 -Queue asks -Port 9077
#>
param(
    [int]$Seconds = 180,
    [string]$Queue = 'casual',   # casual, ranked, lobby or asks (the stand-in games drive it by asks alone)
    [string]$Mode = 'versus',    # versus, sparring (Online Practice: -Queue lobby)
    [string]$Exe = '',           # the game: default launcher\tools\fake_game.py (the stand-in)
    [string]$Game = '',          # with a real -Exe: the game folder whose disc, update and dlc it plays (linked, not copied)
    [string]$Launcher = '',      # default: build\launcher\SC5Launcher.exe
    [string]$CharA = 'Siegfried',
    [string]$CharB = 'Mitsurugi',
    [switch]$PickInGame,         # both players pick on the game's own select screen (the start line says p1=- p2=-)
    [int]$Matches = 1,           # lobby: matches to play there one after the other
    [int]$Rematches = 0,         # casual: rematches the loser chooses after the first match (then LEAVE)
    [string]$Choices = '',       # the loser's choices on the stand-in's result screens in turn (SC5_FAKE_CHOICE:
                                 # rematch, select, leave); default: casual -Rematches x rematch then leave, ranked select
    [int]$AcceptB = 3,           # seconds before B accepts a match found
    [int]$MatchSeconds = 6,      # how long the stand-in's matches last (SC5_FAKE_SECONDS)
    [switch]$Watch,              # lobby: C watches it (a spectator)
    [switch]$Draw,               # every match a draw (SC5_FAKE_RESULT=draw): nobody's win, a set's score stays
    [switch]$Desync,             # every match goes out of step (SC5_FAKE_RESULT=desync): both launchers report it
    [switch]$Stuck,              # the games never take park after a match (SC5_FAKE_STUCK=1): the launchers replace them
    [switch]$Refused,            # each game holds a created character back at its select screen (SC5_FAKE_REFUSED): PICK_REFUSED
    [switch]$OwnLobbyFirst,      # lobby: B is in a lobby of its own when it joins A's (the server moves it)
    [switch]$Relay,              # every match through the server's relay (--relay-only), as with "Hide my IP address":
                                 # the loser's choice and the next match from the result screens all the same
    [int]$ChooseSeconds = 0,     # seconds from the stand-in's result to its loser's choice (SC5_FAKE_CHOOSE_SECONDS,
                                 # default 2; the real game's is 12 s and the loser's own time, 40 s at most)
    [int]$DropAtResult = 0,      # B's server connection drops this many seconds after its first result screen (it
                                 # comes back by itself): the choice made meanwhile goes on, and so does the series

    [string]$DelayA = '',        # [online] input_delay of A and B: auto (default) or 0-4
    [string]$DelayB = '',
    [string]$ServerRoot = '',    # run the server from another checkout's folder (an older release's server/)
    [switch]$Snaps,              # pictures of the launchers' windows (build\online-e2e\snaps)
    [switch]$Visible,            # the launchers' windows on the screen (default: offscreen)
    [int]$Port = 8777            # the server's TCP port (UDP Port + 1), the games' Port + 10 to + 12
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root  # python -m server ... from the checkout
if (-not $Exe) { $Exe = Join-Path $root 'launcher\tools\fake_game.py' }
if (-not $Launcher) { $Launcher = Join-Path $root 'build\launcher\SC5Launcher.exe' }
if (-not (Test-Path $Launcher)) { throw "no launcher at ${Launcher}: build it first (launcher\build.cmd)" }
if ($PickInGame) { $CharA = '-'; $CharB = '-' }
$work = Join-Path $root ($(if ($Port -eq 8777) { 'build\online-e2e' } else { "build\online-e2e-$Port" }))
# A real game's files are linked into the run's game folder (junctions): each link goes on its own first, as a folder
# delete could go through it into the real files
function Remove-GameLinks {
    foreach ($l in 'disc', 'update', 'dlc') {
        $p = Join-Path $work "game\$l"
        if (Test-Path $p) { cmd /c rmdir "$p" | Out-Null }
    }
}
if (Test-Path $work) {
    Remove-GameLinks
    Remove-Item -Recurse -Force $work
}
New-Item -ItemType Directory -Force -Path "$work\a", "$work\b", "$work\c", "$work\server", "$work\snaps", "$work\game" | Out-Null
if ($Game) {
    foreach ($l in 'disc', 'update', 'dlc') {
        if (Test-Path (Join-Path $Game $l)) { cmd /c mklink /J "$work\game\$l" (Join-Path $Game $l) | Out-Null }
    }
}
$py = (Get-Command python).Source
# a throwaway certificate: the launchers pin its fingerprint
& $py -m server.makecert --out-dir "$work\server" | Out-Null
$fp = (& $py -m server.makecert --out-dir "$work\server" --show).Trim()
$i = 0
foreach ($who in 'a', 'b', 'c') {
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
region=eu
max_ping=0
input_delay=$(if ($who -eq 'a' -and $DelayA) { $DelayA } elseif ($who -eq 'b' -and $DelayB) { $DelayB } else { 'auto' })
game_port=$($Port + 10 + $i)
pick_in_game=$(if ($PickInGame) { 'true' } else { 'false' })
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
render_scale=2
anti_aliasing=fxaa
anisotropic=16

[dev]
udp_loopback=true
background=true
"@
    Set-Content -Path "$work\$who\sc5-launcher.ini" -Value $ini -Encoding ascii
    $i++
}
$serverArgs = @('-m', 'server', '--host', '127.0.0.1', '--port', "$Port", '--udp-port', "$($Port + 1)",
                '--db', "`"$work\server\e2e.sqlite`"", '--cert', "`"$work\server\server-cert.pem`"",
                '--key', "`"$work\server\server-key.pem`"")
if ($Queue -eq 'ranked' -or $Queue -eq 'asks') { $serverArgs += '--allow-same-ip' }  # ranked never pairs one public IP otherwise (asks: a real game's script may search ranked)
if ($Relay) { $serverArgs += '--relay-only' }
$server = Start-Process -FilePath $py -ArgumentList $serverArgs -WorkingDirectory $(if ($ServerRoot) { $ServerRoot } else { $root }) -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput "$work\server\out.txt" -RedirectStandardError "$work\server\err.txt"
Start-Sleep -Seconds 2
$vars = @('SC5_DEV_PASSWORD', 'SC5_FAKE_SECONDS', 'SC5_FAKE_RESULT', 'SC5_FAKE_STUCK', 'SC5_FAKE_REFUSED', 'SC5_FAKE_CHOICE',
          'SC5_FAKE_CHOOSE_SECONDS', 'QT_QPA_PLATFORM', 'QT_QPA_FONTDIR')
$env:SC5_DEV_PASSWORD = 'e2e-password-123'  # throwaway accounts on a throwaway server
$env:SC5_FAKE_SECONDS = "$MatchSeconds"
if ($ChooseSeconds -gt 0) { $env:SC5_FAKE_CHOOSE_SECONDS = "$ChooseSeconds" }
$dropB = @()
if ($DropAtResult -gt 0) { $dropB = @('--drop-at-result', "$DropAtResult") }
# the loser's choice on the game's result screen (SCV 0.1.8): a casual series' rematches, then LEAVE; a ranked set's
# matches through CHARACTER SELECT, the winner keeping their fighter
if (-not $Choices -and $Queue -eq 'casual') { $Choices = ((@('rematch') * $Rematches) + @('leave')) -join ',' }
if (-not $Choices -and $Queue -eq 'ranked') { $Choices = 'select' }
if ($Choices) { $env:SC5_FAKE_CHOICE = $Choices }
if ($Draw) { $env:SC5_FAKE_RESULT = 'draw' }
if ($Desync) { $env:SC5_FAKE_RESULT = 'desync' }
if ($Stuck) { $env:SC5_FAKE_STUCK = '1' }
if ($Refused) { $env:SC5_FAKE_REFUSED = 'Hair: it wears downloadable parts the other player''s PC does not have, even without its equipment' }
if (-not $Visible) {
    # the launchers nowhere on the screen: Qt's offscreen platform, which windeployqt leaves out
    $plugin = Join-Path (Split-Path -Parent $Launcher) 'platforms\qoffscreen.dll'
    if (-not (Test-Path $plugin)) {
        $qt = @("$root\external\qt\6.12.0\msvc2022_64", "$env:USERPROFILE\source\repos\mk3d-rollback\external\qt\6.12.0\msvc2022_64") |
            Where-Object { Test-Path "$_\plugins\platforms\qoffscreen.dll" } | Select-Object -First 1
        if (-not $qt) { throw 'qoffscreen.dll not found: pass -Visible, or put Qt in external\qt' }
        Copy-Item "$qt\plugins\platforms\qoffscreen.dll" $plugin
    }
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_QPA_FONTDIR = "$env:WINDIR\Fonts"  # the offscreen platform finds no fonts by itself (the pictures' text)
}
$stamp = Get-Date -Format 'HHmmss'
$users = @{ a = "e2eA$stamp"; b = "e2eB$stamp"; c = "e2eC$stamp" }
function Start-Launcher($who, $extra) {
    if ($Snaps) { $extra += @('--snap-dir', "`"$work\snaps\$who`"") }
    Start-Process -FilePath $Launcher -PassThru -ArgumentList (@('--config', "`"$work\$who\sc5-launcher.ini`"", '--user',
                                                                 $users[$who], '--register') + $extra)
}
if ($Queue -eq 'asks') {
    # the players' part in their games' menus: casual first, then a lobby (the second match)
    $lobbyName = "e2eL$stamp"
    $scriptA = @"
wait account.state=signed_in timeout=60
ask side value=left
wait side=left
ask queue kind=casual
wait queue.state!=idle
ask queue kind=casual
wait message.text~=searching already
wait queue.state=found timeout=60
ask accept
wait game.results>=1 timeout=90
wait series.state=choosing timeout=30
ask leave
wait series.state=over timeout=30
wait game.status=ready timeout=60
ask leaderboard
wait leaderboard.at>=1 timeout=30
ask lobby_ready value=1
wait message.text~=not in a lobby
ask lobby_create name=$lobbyName mode=versus koth=0 private=0
wait lobby.code!= timeout=30
wait lobby.members#>=2 timeout=90
ask lobby_ready value=1
wait lobby.ready=true timeout=30
wait game.results>=2 timeout=120
wait series.kind=lobby
ask lobby_leave
wait lobby=null timeout=30
say done
"@
    $scriptB = @"
wait account.state=signed_in timeout=60
ask accept
wait message.text~=no match is waiting
ask queue kind=casual
wait queue.state=found timeout=60
ask accept
wait game.results>=1 timeout=90
wait series.state=over timeout=60
wait game.status=ready timeout=60
ask lobbies
wait lobbies.list[name=$lobbyName].code!= timeout=60
ask lobby_join code={lobbies.list[name=$lobbyName].code}
wait lobby.members#>=2 timeout=30
ask lobby_ready value=1
wait game.results>=2 timeout=120
ask lobby_leave
wait lobby=null timeout=30
say done
"@
    Set-Content -Path "$work\a\asks.txt" -Value $scriptA -Encoding ascii
    Set-Content -Path "$work\b\asks.txt" -Value $scriptB -Encoding ascii
    $env:SC5_FAKE_ASKS = "$work\a\asks.txt"
    $la = Start-Launcher 'a' @('--play')
    Start-Sleep -Seconds 2
    $env:SC5_FAKE_ASKS = "$work\b\asks.txt"
    $lb = Start-Launcher 'b' @('--play')
    Remove-Item env:SC5_FAKE_ASKS
    $Matches = 2
} elseif ($Queue -eq 'lobby') {
    $lobby = @('--lobby-name', "e2e$stamp", '--lobby-matches', "$Matches", '--lobby-code-file', "`"$work\lobby-code.txt`"",
               '--mode', $Mode)
    $la = Start-Launcher 'a' (@('--lobby', 'host', '--character', $CharA) + $lobby)
    Start-Sleep -Seconds 3
    $extraB = @()
    if ($OwnLobbyFirst) { $extraB += '--lobby-own-first' }

    $lb = Start-Launcher 'b' (@('--lobby', 'join', '--character', $CharB) + $lobby + $extraB + $dropB)
} else {
    $search = @('--queue', $Queue, '--mode', $Mode, '--rematches', "$Rematches")
    $la = Start-Launcher 'a' ($search + @('--character', $CharA, '--auto-accept', '2'))
    Start-Sleep -Seconds 2
    $lb = Start-Launcher 'b' ($search + @('--character', $CharB, '--auto-accept', "$AcceptB") + $dropB)
}
$lc = $null
if ($Watch) {
    Start-Sleep -Seconds 2
    $lc = Start-Launcher 'c' @('--lobby', 'watch', '--lobby-name', "e2e$stamp", '--lobby-code-file', "`"$work\lobby-code.txt`"",
                               '--mode', $Mode)
}
foreach ($k in $vars) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
$ls = @($la, $lb) + @($lc | Where-Object { $_ })
"server pid $($server.Id), launchers $(($ls | ForEach-Object { $_.Id }) -join ' '), accounts $($users.a) / $($users.b)$(if ($lc) { " / $($users.c) (watching)" })"
if ($Queue -eq 'casual' -and $Rematches -gt 0) { $Matches = 1 + $Rematches }
$t0 = Get-Date
$seen = @{}
$watched = 0
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
    foreach ($ln in $serverLog | Select-String -Pattern 'RESULT|DRAW|NO_CONTEST|SET |series|DISPUTE|ABANDONED|DESYNC_PAIR|PICK_REFUSED|Traceback') {
        if (-not $seen[$ln.Line]) { $seen[$ln.Line] = 1; "{0,6:N1}s server: {1}" -f ((Get-Date) - $t0).TotalSeconds, ($ln.Line -replace '^\S+ \S+ \S+\s+', '') }
    }
    # the spectator's games' ends (each game logs every status line it wrote: a short one can fall between two polls)
    $watched = @(Get-ChildItem -Path "$work\c\sessions" -Filter game.log -Recurse -ErrorAction SilentlyContinue |
                 Select-String -Pattern 'link: status (result|sparring_over)').Count
    $done = @($serverLog | Select-String -Pattern '(RESULT|DRAW|NO_CONTEST) match=').Count
    # a ranked set ends at its winner's third match won; draws never end one: -Draw counts its matches
    $finished = if ($Queue -eq 'ranked' -and -not $Draw) { @($serverLog | Select-String -Pattern 'SET \S+ over').Count -ge 1 } else { $done -ge $Matches }
    if ($Mode -eq 'sparring') {
        # Online Practice records nothing (no RESULT line): its matches end with the games' sparring_over, and the next
        # one counts down by itself, both players staying ready
        $finished = @(Get-ChildItem -Path "$work\a\sessions" -Filter game.log -Recurse -ErrorAction SilentlyContinue |
                      Select-String -Pattern 'link: status sparring_over').Count -ge $Matches
    }
    if ($Watch) { $finished = $finished -and $watched -ge $Matches }  # the spectator runs behind
    if ($Queue -eq 'asks') {
        # both stand-in games went through their scripts (or one failed: no use waiting)
        $said = @(Get-ChildItem -Path "$work\a\sessions", "$work\b\sessions" -Filter game.log -Recurse -ErrorAction SilentlyContinue |
                  Select-String -Pattern 'asks script: (done|FAILED)')
        # (not -match: it would overwrite $Matches, this script's parameter)
        $failed = @($said | Where-Object { $_.Line.Contains('FAILED') })
        if ($failed.Count) { $finished = $false; "asks script failed: $($failed[0].Line)"; break }
        $finished = @($said | Where-Object { $_.Line.Contains('script: done') }).Count -ge 2
    }
    if ($finished) { Start-Sleep -Seconds 8; break }
    if (@($ls | Where-Object { $_.HasExited }).Count) { "a launcher exited"; break }
    Start-Sleep -Milliseconds 250
}
$who = @('a', 'b') + $(if ($Watch) { @('c') } else { @() })
"--- what the launchers did (search, match found, accept, match, result, rematch, set, lobby, watch)"
foreach ($s in $who) {
    Get-Content "$work\$s\sc5-launcher.log" -ErrorAction SilentlyContinue |
        Select-String -Pattern 'cli:|bridge:|match: |series|Match found|MATCH FOUND|accept|rematch|Rematch|watch|Watch|error|lobby|notice|dropping|logged in' |
        Select-Object -Last 40 | ForEach-Object { "[$s] $($_.Line)" }
}
"--- the games' start lines and the variables they were given"
foreach ($s in $who) {
    Get-ChildItem -Path "$work\$s\sessions" -Directory -ErrorAction SilentlyContinue | Select-Object -First 1 | ForEach-Object {
        $log = Join-Path $_.FullName 'game.log'
        if (Test-Path $log) { Select-String -Path $log -Pattern 'env: ' | Select-Object -First 1 | ForEach-Object { "[$s] $($_.Line)" } }
    }
    Get-ChildItem -Path "$work\$s\sessions" -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $log = Join-Path $_.FullName 'game.log'
        if (Test-Path $log) { Select-String -Path $log -Pattern 'link: start|asks script' | ForEach-Object { "[$s] $($_.Line)" } }
    }
}
"--- server log (matches and results)"
Get-Content "$work\server\err.txt", "$work\server\out.txt" -ErrorAction SilentlyContinue |
    Select-String -Pattern ' go: |RESULT|DRAW|NO_CONTEST|DESYNC_REPORT .* lines|DESYNC_PAIR|elo|set |series|lobby|spectat|feed' | Select-Object -Last 40 | ForEach-Object { $_.Line -replace '^\S+ \S+ \S+\s+', '' }
# close only what this run started: the launchers and the games they started, then the server
$games = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.ParentProcessId -in @($ls | ForEach-Object { $_.Id }) } | ForEach-Object { $_.ProcessId })
foreach ($p in $ls) { if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null } }
Start-Sleep -Seconds 3
foreach ($id in @($ls | ForEach-Object { $_.Id }) + $games) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
if (-not $server.HasExited) { Stop-Process -Id $server.Id -Force }
Start-Sleep -Seconds 1
Remove-GameLinks  # the run's folder stays for a look, without links into the real game's files
"--- database"
& $py -I -c @"
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
print('accounts', c.execute('SELECT name FROM accounts ORDER BY name').fetchall())
for t in ('ratings', 'stats', 'records', 'elo'):
    try:
        print(t, c.execute(f'SELECT * FROM {t}').fetchall()[:20])
    except sqlite3.Error:
        pass
print('matches', c.execute('SELECT id, mode, ranked, status, winner, p1_rounds, p2_rounds FROM matches ORDER BY created').fetchall())
"@ "$work\server\e2e.sqlite"
"still running from this run: $(@(Get-Process -Id (@($ls | ForEach-Object { $_.Id }) + $games + $server.Id) -ErrorAction SilentlyContinue).Count)"
# the result screens: the loser's choices the games made, and the next matches that started from there
$serverAll = @(Get-Content "$work\server\err.txt", "$work\server\out.txt" -ErrorAction SilentlyContinue)
$inPlace = @($serverAll | Select-String -Pattern 'at the result screens').Count
$relayed = @($serverAll | Select-String -Pattern 'relayed through this server').Count
$chosen = @(Get-ChildItem -Path "$work\a\sessions" -Filter game.log -Recurse -ErrorAction SilentlyContinue |
            Select-String -Pattern 'link: status chose').Count
"result screens: $chosen choices in A's game, $inPlace next matches from there$(if ($Relay) { ", $relayed matches relayed" })"
$why = "not finished within $Seconds s"
if ($finished -and $Relay -and $Matches -gt 1 -and $Mode -ne 'sparring' -and ($inPlace -lt 1 -or $relayed -lt 1 -or $chosen -lt 1)) {
    $finished = $false
    $why = 'through the relay, no next match came from the result screens'
}
if ($finished -and $DropAtResult -gt 0) {
    $bLog = @(Get-Content "$work\b\sc5-launcher.log" -ErrorAction SilentlyContinue)
    $dropped = @($bLog | Select-String -Pattern 'dropping the connection').Count
    $logins = @($bLog | Select-String -Pattern 'server: logged in as').Count
    "B dropped its connection $dropped time(s) and signed in $logins time(s)"
    if ($dropped -lt 1 -or $logins -lt 2) { $finished = $false; $why = 'B never dropped and came back at the result screen' }
}
if ($finished) { "PASS: $Queue $Mode$(if ($Watch) { ', watched' })$(if ($Draw) { ', draws' })$(if ($Relay) { ', relayed' })$(if ($DropAtResult) { ', a drop at the result screen' })" } else { "FAIL: $why" }
