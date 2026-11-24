# SCV Online protocol (v1)

This is the specification that `server/` (Python, asyncio) and `launcher/` (Qt 6) implement, and that the game side
in `sc5/` (`sc5.exe`, the ReXGlue build of Soulcalibur V) implements for its half of section 3. It is versioned by
one integer, `PROTO = 1`; any incompatible change bumps it.

Sections 1 and 2 are MK3D Online's protocol (`mk3d-rollback/docs/PROTOCOL.md`), which SCV Online's server and
launcher are ported from, with SCV's names; a version in brackets, such as (0.1.8) or (1.0.7), is the MK3D Online
release a feature came with: SCV Online 0.1.0 has every one of them. Section 3, the launcher and the game, is SCV's:
MK3D's with the `SC5_` prefix, the Xbox 360 game's folders and the display settings of its GPU emulation.

Three links are involved:

```
 launcher A  -- TLS/TCP, NDJSON -->  server  <-- TLS/TCP, NDJSON --  launcher B
     |  ^          (section 1)        ^  |           (section 1)          |  ^
     |  +--- UDP hello / hello_ack ---+  +---- UDP hello / hello_ack -----+  |
     |                               (section 2)                             |
     +--------------- UDP punch + ping, then the game's own traffic ---------+
     |                                                                       |
  game A  <-- env + cmd.txt / status.txt -->  launcher A   (section 3)    game B
```

The game and its modes (ids are used on every link):

| game id | title | online modes | offline |
|---|---|---|---|
| `sc5` | Soulcalibur V (Xbox 360, World, title `4E4D083D`, with Title Update 1) | `versus` (Ranked, Casual, lobbies), `sparring` (Online Practice: lobbies only) | `practice` |

The display names and the roster live in `launcher/data/sc5.json`; the server's list is `server/protocol.py`
`GAMES = {"sc5": ("versus", "sparring")}`. The ranked titles by Elo (`server/elo.py`, `welcome.ranks`), lowest
first: Novice (below 575), Apprentice 575, Squire 675, Wanderer 775, Duelist 875, Knight 975, Vanguard 1075,
Blademaster 1175, Champion 1275, Conqueror 1375, Hero 1475, Legend 1575, Soul Warrior 1675, Soul Master 1795,
Spirit Sword 1915, Eternal Soul 2035; a new player starts at 475.

---

## 1. Client <-> server

### 1.1 Transport

- One TCP connection per launcher, TLS 1.2 or newer (the server prefers 1.3).
  Default port **7777**.
- The server uses a **self-signed certificate** (`python -m server.makecert`).
  The launcher does not use a CA: it pins the **SHA-256 fingerprint of the
  DER-encoded certificate** (64 hex digits; colons, spaces and case are
  ignored) and accepts the handshake only if the presented certificate
  matches. Host-name and chain errors are ignored when, and only when, the
  fingerprint matches. With no fingerprint configured the launcher shows the
  presented one and asks the user whether to trust it (stored afterwards:
  trust on first use).
- Framing: **newline-delimited JSON**. Every message is one JSON object encoded
  as UTF-8 on a single line terminated by `\n` (a preceding `\r` is
  tolerated). Maximum line length 16 384 bytes (a `desync_report` 65 536,
  1.13.1); the server closes connections that exceed it.
- Every object has a `"type"` string. Unknown fields are ignored by both sides
  (forward compatibility). Unknown types get `error{code:"unknown_type"}`.
- A request may carry `"id"` (any JSON scalar). The direct reply to that
  request, or the `error` it caused, echoes the same `"id"`. Unsolicited
  server messages carry no `id`.
- Keepalive: the client sends `ping` every 30 s; the server drops a connection
  that has sent nothing for 120 s.
- A development-only plain-TCP mode exists (`python -m server --plain`,
  launcher address `tcp://host:port`); never use it on the internet.

### 1.2 Errors

```json
{"type":"error","code":"bad_login","message":"Wrong name or password.","re":"login","id":7}
```

`re` names the request type that failed. Codes used in v1:

| code | meaning |
|---|---|
| `bad_json`, `too_long`, `unknown_type`, `bad_request` | malformed input (field missing or out of range) |
| `proto_mismatch`, `client_outdated` | reply to `hello`; the server then closes |
| `need_hello` | anything but `hello` before `welcome` |
| `not_logged_in` | the request needs an account |
| `bad_name`, `bad_password`, `name_taken` | `register` validation |
| `bad_login`, `bad_token`, `rate_limited` | `login` / `auth` |
| `logged_in_elsewhere` | the account logged in on another connection; this one is closed |
| `bad_game`, `bad_mode`, `bad_region` | unknown game, mode or region |
| `busy` | already queued, in a lobby or in a match |
| `no_such_match`, `no_such_lobby`, `lobby_full`, `bad_lobby_password`, `not_host`, `not_ready`, `not_in_lobby` | match / lobby requests |
| `update_needed` | a King of the Hill lobby asked of a launcher whose `hello` did not say it has them (1.10.2) |
| `rating_closed`, `not_rater`, `already_rated` | `lobby_rate` (1.10.2) |
| `slow_down` | chat sent faster than the limit |
| `not_admin` | `admin_notice` from a normal account |
| `no_such_account` | `profile` for an unknown name |
| `idle`, `internal` | connection idle for 120 s (then closed); unexpected server error |

### 1.3 Session start and version check

```json
C: {"type":"hello","proto":1,"client":"sc5-launcher","version":"0.1.0","os":"windows","features":["koth"],
    "build":"2026-11-23 22:31"}
S: {"type":"welcome","proto":1,"server":"sc5-server","version":"0.1.0","udp_port":7778,
    "motd":"Welcome!","online":12,"min_client":"0.1.0",
    "games":{"sc5":["versus","sparring"]},
    "regions":["na","sa","eu","me","af","as","oc"],"direct_above_ms":100,
    "ranks":[{"title":"Novice","from":null},{"title":"Apprentice","from":575},
             {"title":"Squire","from":675}, ...,{"title":"Eternal Soul","from":2035}],
    "features":["koth","desync_detail"]}
```

- `features` (King of the Hill): what each side has beyond the protocol's
  start. A launcher lists what it can do in `hello` (one without it has none);
  the server lists what it has in `welcome`. `koth`: King of the Hill lobbies
  and their Respect Points (1.10.2). The launcher shows its King of the Hill
  box and the Leaderboard's Respect only when the server lists it.
  `desync_detail` (SCV 0.1.4, the server's alone): desync reports of up to 200
  lines in a message of up to 64 KB (1.13.1); a launcher whose server does not
  list it sends the 36 lines an older server takes.

- `proto` must equal the server's `PROTO`, else `error{code:"proto_mismatch"}`.
- `ranks` (0.1.21): the ranked titles by Elo, lowest first, each held from its
  `from` up to the next one's (the first, `null`, below the second's):
  Novice below 575 (a new player starts at 475), Apprentice 575, Squire 675,
  Wanderer 775, Duelist 875, Knight 975, Vanguard 1075, Blademaster 1175,
  Champion 1275, Conqueror 1375, Hero 1475, Legend 1575, Soul Warrior 1675,
  Soul Master 1795, Spirit Sword 1915, Eternal Soul 2035: 100 Elo a rank, 120
  from Soul Warrior on (server/elo.py `RANKS`, each with its badge). Every ranked
  rating the server sends comes with its `title` (1.5,
  1.7, 1.8, 1.9.1, 1.11), and so does every player in a lobby or a match,
  ranked or not (1.8 `match_go`, 1.10, 1.10.1); the launcher shows the ladder
  in tooltips, the next title on the Ranked page and each title's badge
  (launcher/data/ranks, by the title in lower case with dashes for spaces:
  `soul-warrior.png`, drawn by launcher/tools/make_art.py); the start line
  passes both players' to the game (3.3 `p1_rank`).
- `direct_above_ms` (0.1.18): a match relayed only because a player hides their
  address tries a direct connection when its ping through the relay is over
  this (1.7 `relay`; 0: never), for the launcher's Settings to say.
- `version` is compared component-wise with `min_client` (server option
  `--min-client`); older clients get `error{code:"client_outdated"}`, whose
  `message` ends with where to download the release: the update file's
  `page`, else the server's `--releases-page`, else nothing ("Update the
  launcher to 0.1.1 or newer."). Both errors close the connection. SCV Online
  has its own server instance, version gate and update file: MK3D Online's
  server serves MK3D's launchers alone.
- **Updates** (0.1.7). The server's update file (`--update-file`, by default
  `update.json` beside the database; read again when it changes, a bad one is
  logged and ignored) names the latest release:

  ```json
  {"version":"0.1.8",
   "url":"https://example.org/releases/download/v0.1.8/SC5-Online-0.1.8-windows.zip",
   "sha256":"<the zip's, 64 lowercase hex>","size":96000000,"notes":"What is new, a few lines.",
   "page":"https://example.org/releases/tag/v0.1.8"}
  ```

  A client older than it gets it as `welcome.update`, and in the
  `client_outdated` error as `update` when the release is new enough to let
  it in. Once the file names a new release, connected clients older than it
  get `{"type":"update", ...the same fields}` within
  `update_check_interval` (10 s). `url` is `https://` (or `http://` to
  127.0.0.1 / localhost, for tests); the launcher checks the zip's size and
  SHA-256 against these before it installs anything (docs/LAUNCHER.md,
  Updates). On the official server the file is `/var/lib/sc5/update.json`
  (`server/deploy/install.sh`).
- **Builds** (SCV 0.1.4). A version number alone does not tell two packages
  apart: a test build given out before a release took its number is that
  number too, and was never offered the release (2026-11-20: a player on a
  private test build of 0.1.3 was not offered 0.1.3). A player package's
  launcher says which build it is in `hello`, `"build":"2026-11-23 22:31"`:
  its `VERSION.txt` stamp (`SC5-Online 0.1.4, packaged 2026-11-23 22:31`; a
  source build sends none), 1-40 characters `[0-9A-Za-z :._-]` (anything else
  is taken as none). The update file may name the release's own stamp,
  `"build":"2026-11-20 06:17"` (the same characters; `tools\package.ps1`
  prints it), which then comes with every offer: a launcher of the release's
  version whose build differs from it (both known) is offered the release as
  an older one is, in `welcome` and pushed, and its launcher takes it although
  the number is its own. A newer launcher (a developer's) is offered nothing;
  without `build` in the file the version alone decides, as before. The
  server logs a login's build with its version.
- Regions: `na` North America, `sa` South America, `eu` Europe, `me` Middle
  East, `af` Africa, `as` Asia, `oc` Oceania.

### 1.4 Accounts

Names: 3-16 characters `[A-Za-z0-9_-]`, unique case-insensitively. Passwords:
6-128 characters. The server stores `scrypt$n$r$p$<salt hex>$<hash hex>`,
computed with `hashlib.scrypt(password, salt=<16 random bytes>, n=16384, r=8,
p=1, dklen=32)` and compared in constant time. Ten failed `login`/`auth`
attempts from one IP within 5 minutes give `rate_limited` until the window
has passed.

```json
C: {"type":"register","name":"Taki","password":"..."}
C: {"type":"login","name":"Taki","password":"..."}
C: {"type":"auth","token":"<64 hex>"}            (resume with a remembered token)
C: {"type":"logout"}                             (revokes the token server-side)
S: {"type":"account","name":"Taki","token":"<64 hex>","token_expires":1767225600,
    "udp_token":"<32 hex>","admin":false,"profile":{"...":"as in 1.5"}}
S: {"type":"logged_out"}
```

- `register` logs the new account in (same `account` reply).
- `token` is a persistent login token (30 days, stored only as its SHA-256 on
  the server). The launcher keeps it when "remember me" is checked.
- `udp_token` is a **per-connection session token** for the UDP rendezvous
  (section 2). It is not a login credential (it cannot be used with `auth`),
  so seeing it on the wire does not expose the account.
- Logging in on a second connection closes the first one with
  `error{code:"logged_in_elsewhere"}`.

### 1.5 Profile

```json
C: {"type":"profile"}                    (own profile; or {"type":"profile","name":"Taki"})
S: {"type":"profile","name":"Taki","created":1761300000,
    "stats":{"sc5":{"versus":{"ranked":{"elo":1532,"title":"Wanderer","wins":4,"losses":2},
                              "casual":{"wins":10,"losses":7}}}}}
```

Stats exist per game per mode: ranked Elo (start 475, moved per set) with
its title (1.3 `ranks`, 0.1.21) and ranked wins/losses, which count sets
(1.9.1; since 1.0.1, matches before), and casual wins/losses, which count
matches. A mode never played ranked comes with its start (Elo 475, a Beginner,
0-0). A mode where King of the Hill spectators rated the player's wins has
`"respect":{"points":26,"ratings":4}` too (1.10.2).

### 1.6 Online count and notices

```json
C: {"type":"online"}
S: {"type":"online","count":12,"connected":14,"queued":3,"playing":4,
    "queues":{"sc5/versus/casual":2,"sc5/versus/ranked":1},
    "games":{"sc5":{"playing":9,"searching":3,"matches":4,"lobbies":3}},
    "searchers":[{"name":"Kitana","game":"sc5","queue":"sc5/versus/casual"},...]}
S: {"type":"notice","text":"Server restarts in 10 minutes.","level":"warn"}
C: {"type":"admin_notice","text":"...","level":"info"}      (admins only, server --admins)
```

`count` = logged-in accounts, `connected` = open connections, `playing` =
players in matches past the accept step. `queues` counts the searches in each
queue (`<game>/<mode>/<casual|ranked>`). `games` (0.1.13) counts each game's players: `searching`,
in its `matches` (from `match_found` on, and between two matches of a series),
in its `lobbies` (spectators too), and `playing`, any of those, each player
once; the matchmaking pages show them. `searchers` (0.1.14) names who searches,
with the game and the queue, the longest waiting first (40 at most); the
matchmaking pages list them game by game. The server pushes `online` to every
connection when the numbers change, at most once per 2 s. `notice` is pushed
to everyone; `welcome.motd` carries the message of the day. `level` is
`info` or `warn`.

### 1.7 Matchmaking queue

```json
C: {"type":"queue_join","game":"sc5","mode":"versus","ranked":false,"region":"eu",
    "max_ping":150,"strict":false,"character":"Siegfried","delay":2,"server_rtt":24,"relay":false,
    "link":"ethernet"}
S: {"type":"queued","game":"sc5","mode":"versus","ranked":false,"region":"eu",
    "max_ping":150,"strict":false,"since":1761300000.5,"in_queue":3,"elo":1500,"title":"Squire",
    "ping_widen":{"after":30.0,"every":15.0,"step":25,"cap":250}}
C: {"type":"queue_leave"}
S: {"type":"queue_left","reason":"left"}
```

- `max_ping` is the highest acceptable round trip in ms; `0` means any. A
  search that finds nobody raises it (0.1.8, unless `strict`): after `after`
  seconds by `step` ms, again every `every` seconds, up to `cap` ms (never
  below the player's own). `queued.ping_widen` gives the rule (absent for
  `0` or `strict`) so the launcher can show the raised limit; a match keeps
  the limit it was made with, its rematches too.
- `link` (optional, 0.1.8): the network this PC reaches the server through,
  `ethernet`, `wifi` or `other` (anything else is dropped); shown to the
  opponent with `match_found.connection`.
- `delay` is the player's preferred input delay in frames (0-4). Auto (SCV
  0.1.4, the launcher's default): `"delay_auto":true` (the server also takes
  MK9 Online's `"delay":"auto"`) with a number for an older server, which
  ignores the flag (SCV launchers send 1): the match's delay then comes from
  its measured round trip (1.8 `match_go`).
- `death_traps` (optional, default `true`): MK3D Online's arenas' death traps, which the server still takes and
  passes on (`match_found`, `match_go`, `spectate`, lobbies). Soulcalibur V has nothing it would turn off: SCV
  launchers never send it, and the start line (3.3) has no key for it.
- `server_rtt` (optional) is the client's last measured UDP round trip to the
  server in ms (section 2.2); the UDP hellos sent while queued update it. It
  orders otherwise equal ranked candidates, and for a match that would go
  through the relay (below) the sum of both players' figures is its expected
  ping: a pair above either player's `max_ping` is not offered a match.
- `relay` (optional, default `false`, 0.1.8): the player hides their IP
  address (the launcher's Settings, "Hide my IP address from opponents"). Any
  match with such a player goes through the server's relay (2.4) from the
  start, so neither player is ever sent the other's address; it costs ping for
  players far from the server. A ranked set or a casual series keeps the
  choice its first match was made with. Since 0.1.18 a match relayed only for
  that, whose ping through the relay is over the server's `direct_above_ms`
  (`welcome`, 100 by default, 0: never), tries a direct connection (section
  2.4, step 5 of 1.8): the two then learn each other's address.
- `hide_always` (optional, default `false`, 0.1.18; with `relay` alone): the
  player hides their address however high the relay's ping: a match with them
  is never tried directly (the launcher's "Even when the ping through the
  server is high").
- `character` is a roster id from the launcher data (`launcher/data/sc5.json`, e.g. `Mitsurugi`); empty or `-`
  for a player who picks on the game's own select screen (the launcher's default).
- One queue entry per account; joining again replaces the entry. A player in
  a lobby or a match gets `busy`. `queue_left.reason` is `left` or
  `removed` (declined or let the accept window run out).
- **Casual pairing (first come, first served).** The queue is scanned in join
  order; each waiting player is paired with the earliest-joined compatible
  player. Compatible = same game and mode, both casual, different accounts,
  same region (or, if neither is `strict`, different regions once both have
  waited 20 s), and no cached ping for this pair above either player's
  `max_ping` (ping cache below; for a relayed pair with no measurement, the
  sum of their `server_rtt`).
- **Ranked pairing (Elo window).** The same rules plus an Elo window: a pair
  is allowed when `|elo_a - elo_b| <= min(window_a, window_b)`, with
  `window = 100 + 50 * floor(waited / 10 s)`, capped at 1100. `strict` pins the
  window at 100 and the region to the player's own (a mode the server lists in
  `MatchmakingConfig.any_elo_modes` has no window: MK3D's side modes; SCV lists
  none). Among the allowed
  partners of the longest-waiting player the server takes the smallest Elo
  difference, then the lowest estimated ping (sum of both players' UDP round
  trips to the server, a rough proxy), then the earliest join time. Ranked
  never pairs two connections from the same public IP (server option
  `--allow-same-ip` lifts this for LAN events and development).
- **Ping cache (measured ping).** Every `ping_report` stores the measured
  round trip for that pair of accounts for 10 minutes; a pair whose punch
  failed is stored as unreachable. The matchmaker skips cached pairs that are
  unreachable or above either player's `max_ping`. When a match is cancelled
  for a ping or punch reason, both players go back to the queue with their
  original join time.
- The matchmaker runs on every `queue_join` and once per second.

### 1.8 Match setup (queue and lobby matches)

```
 found --accept x2--> accepted --fresh UDP hello x2--> punching --ping_report x2--> playing --result x2--> done
   | decline / 15 s      | 10 s                           | fail / too slow / 15 s       | 120 s after the 1st report
   +------------------> cancelled <-----------------------+                              +--> unconfirmed
```

1. **Found.**
   ```json
   S: {"type":"match_found","match":"5f2c9a01d4e3","game":"sc5","mode":"versus","ranked":false,
       "side":1,"accept_timeout":20,"accept_required":true,"lobby":null,"death_traps":true,
       "opponent":{"name":"Kilik","elo":null,"title":null,"region":"eu","character":"Astaroth"},
       "connection":{"link":"wifi","ping":42,"estimate":null,"measured":false}}
   ```
   In ranked, `opponent` is `null` until both have accepted (no dodging by
   name). `connection` (0.1.8, ranked too) is the other player's: `link` from
   their search (`null` unknown), `ping` their round trip to the server
   (`server_rtt`), `estimate` the round trip between the two when the server
   knows it (`measured:true`: from their match within the ping cache's 10
   minutes; else, for a match through the relay, both ways to the server). Lobby matches have `accept_required:false` and `accept_timeout:0`
   (the players already readied up) and continue at step 3.
2. **Accept window: 15 s.**
   ```json
   C: {"type":"match_accept","match":"5f2c9a01d4e3"}
   C: {"type":"match_decline","match":"5f2c9a01d4e3"}
   ```
   A decline or a missing accept cancels the match. The player who declined
   or did not answer leaves the queue (`queue_left{reason:"removed"}`); the
   other goes back with their original join time (`requeued:true`).
3. **UDP endpoints.** On `match_found` the launcher binds its game UDP port
   and sends UDP `hello` packets (section 2) until `hello_ack` arrives. The
   server needs a hello from each side received after `match_found`; when
   both have accepted and both endpoints are known it sends:
   ```json
   S: {"type":"match_peer","match":"5f2c9a01d4e3","side":1,"key":"<32 hex>","punch_timeout":10,
       "same_nat":true,
       "peer":{"public":"203.0.113.7:7001","lan":"192.168.1.20:7001"},
       "opponent":{"name":"Kilik","elo":1488,"title":"Squire","region":"eu","character":"Astaroth"}}
   ```
   In ranked the opponent's `elo` (as they searched) and its `title` come with
   it (0.1.21; `null` in casual).
   No IP address is revealed before both players have accepted, and `lan`
   only to a player behind the same public address (`same_nat`; empty
   otherwise). A match with a player who hides their address (`relay` in
   `queue_join` or `lobby_ready`), or every match on a server run with
   `--relay-only`, skips this step: both get the relay's `match_peer` (step 5)
   at once and never learn each other's address. So does a match with a player
   whose hellos got no answer and who asked for the tunnel instead
   (`udp_tunnel`, section 2.4.1): that stands for their endpoint. If an
   endpoint is still missing 10 s after acceptance the match is cancelled
   (`no_udp`).
4. **Punch and measure** (section 2.3), then:
   ```json
   C: {"type":"ping_report","match":"5f2c9a01d4e3","ok":true,"rtt_ms":38,
       "remote":"203.0.113.7:7001","via":"public"}
   C: {"type":"ping_report","match":"5f2c9a01d4e3","ok":false,"rtt_ms":0}
   ```
5. **Go, relay or cancel.** When both reports are in:
   - (0.1.18) both `ok` through the relay for a match relayed only because a
     player hides their address (none with `hide_always`, nobody tunnelled),
     with the worse `rtt_ms` over `direct_above_ms`: the two try each other
     directly once: both get a direct `match_peer` (the other's endpoints, as
     in step 3) with `"direct_why":"relay_ping"`, `relay_rtt` (the ping through
     the relay) and `relay_mine` (true for a player who hides their address:
     theirs shows now), punch and report again. Faster than the relay by
     `direct_gain_ms` (20) or more: `match_go` direct. Otherwise, or with either
     `ok:false`: back through the relay, a relay `match_peer` again with
     `"back":true`, punch and report again (no second direct try);
   - either `ok:false`, the first time: the match moves to the server's relay
     (section 2.4): both get a second `match_peer` with `"relay":true` and
     empty `peer` endpoints, punch and ping through the server's UDP port, and
     report again (`via:"relay"`, `remote` = the server's UDP endpoint);
   - either `ok:false` again, or with the relay off (`--no-relay`):
     `match_cancelled{reason:"punch_failed"}`;
   - the worse `rtt_ms` above the smaller non-zero `max_ping` of the two:
     `match_cancelled{reason:"ping_too_high"}`;
   - otherwise both get `match_go`:
   ```json
   S: {"type":"match_go","match":"5f2c9a01d4e3","game":"sc5","mode":"versus","ranked":false,
       "side":1,"remote":"203.0.113.7:7001","rtt_ms":38,"delay":2,"seed":2718281828,"rounds":3,
       "p1":"Siegfried","p2":"Astaroth","p1_name":"Taki","p2_name":"Kilik","p1_title":"Wanderer",
       "p2_title":"Squire","lobby":null,"death_traps":true,"feed":false}
   ```
   `p1_title`, `p2_title`: each player's ranked title in the match's game and
   mode, in a casual or lobby match too (someone who never played it ranked:
   the starting one; 1.3 `ranks`), for the badges the games draw beside the
   timer (3.3 `p1_rank`). `delay` = the larger of both
   preferences, an Auto one's (1.7) from `rtt_ms` (the larger of the two
   players' measured round trips, the same for both games:
   `protocol.auto_delay`, the one-way trip's frames rounded up, less one,
   1 to 6: 1 under 67 ms, 2 to 100, 3 to 133, 4 to 166, 5 to 200, then 6),
   at least 1 in ranked; both players get the same, and `"delay_auto":true`
   when an Auto one took part (SCV 0.1.4; a launcher that finds no
   `delay_auto` in `match_go`, an older server's, raises the delay for the
   ping itself when its player is on Auto). Before Auto every match of the
   night of 2026-11-20 ran at delay 1 (the launcher's raise was MK3D
   Online's, the one-way frames rounded less one: 1 up to 83 ms), a US-UK
   pair among them, which rolled back up to 5 frames. `seed` is
   a fresh 32-bit value. `death_traps`: always `true` with SCV launchers (1.7).
   `feed`: a lobby match, whose spectators watch it from what both games write
   (1.10.1). Sides are random for queue matches; in a lobby the
   host is side 1. `remote` echoes the endpoint this side reported. A missing
   report 15 s after `match_peer` cancels with `ping_timeout`.
6. **Cancellation** at any step before `playing`:
   ```json
   S: {"type":"match_cancelled","match":"5f2c9a01d4e3","reason":"declined","requeued":true}
   ```
   Reasons: `declined`, `accept_timeout`, `no_udp`, `punch_failed`,
   `ping_too_high`, `ping_timeout`, `peer_left`. `requeued` tells this
   player whether they are back in the queue.
7. **Playing.** The launcher writes the `start` line (section 3) and may tell
   the server what the game does:
   `{"type":"match_state","match":"...","state":"in_match"}` (informational).

### 1.9 Result reporting

Both launchers report what their game wrote to `status.txt`:

```json
C: {"type":"result","match":"5f2c9a01d4e3","winner":1,"p1_rounds":2,"p2_rounds":1,"p1_char":"12.1","p2_char":"7.0"}
C: {"type":"result","match":"5f2c9a01d4e3","winner":0,"p1_rounds":1,"p2_rounds":1,"p1_char":"3.0","p2_char":"21.1"}
C: {"type":"result","match":"5f2c9a01d4e3","outcome":"desync","frame":1234}
C: {"type":"result","match":"5f2c9a01d4e3","outcome":"disconnected"}
C: {"type":"result","match":"5f2c9a01d4e3","outcome":"left","exit":"window closed","exit_code":0}
C: {"type":"result","match":"5f2c9a01d4e3","outcome":"sparring"}
S: {"type":"result_ack","match":"5f2c9a01d4e3"}
S: {"type":"match_result","match":"5f2c9a01d4e3","status":"agreed","winner":1,
    "p1_rounds":2,"p2_rounds":1,"ranked":true,"left":null,"left_side":null}
```

- `winner` is 1 or 2, or 0 for a drawn match (the game's: a double KO, or the
  last round even on time; docs/GAME.md "The launcher's game"); anything else
  is refused (`bad_request`).
- **Draw** (both report `winner` 0 with the same rounds): status `draw`, with
  `winner` 0 in `match_result` and `spectate_end`. It counts for nobody: no
  casual win or loss; in a ranked set no point for either player, its next
  match going as the drawn one did (the last winner keeps their fighter), and
  draws never end a set as matches that did not count do (`void`); in King of
  the Hill the same two play again and nobody rates it (1.10.2). The server
  logs a `DRAW` line.
- **Agree** (same `winner`, `p1_rounds`, `p2_rounds`): status `agreed`; a
  casual match counts as a win and a loss (casual wins/losses), a ranked one
  counts in its set, whose end moves the ranked wins/losses (sets, since
  1.0.1) and Elo, not each match (1.9.1).
- `p1_char`/`p2_char` (optional, from the game's `result` line: its own
  character ids, absent in modes without fighters) are the match's fighters,
  from 0.1.21 with the costume they fought in after a dot (`12.1`: character
  12, the game's costume word 1). When both reports name the same ones, a
  ranked set's next match gives its winner that fighter and a casual rematch
  can start with them (1.9.1); they never decide the result, and a value the
  server does not take is ignored.
- **Disagree**: status `disputed`; nothing changes, both reports stay in the
  database and the server logs a `DISPUTE` line.
- **Desync or disconnect** reports: status `no_contest`; nothing changes;
  logged.
- **Online Practice** (1.0.7, mode `sparring`): the game reports
  `sparring_over` once a player went back to the select screen (BACK), the
  launcher the outcome `sparring` (refused, `bad_request`, for any other
  mode's match); both such reports give status `practice`, which counts
  nothing anywhere.
- **Only one report** within 120 s of the first (20 s when it says the games
  lost each other: `disconnected` or `error`): status `unconfirmed`; nothing
  changes; logged.
- **Left**: a launcher reports `left` when its own game closed while the
  match ran (Alt+F4, a crash, its window closed, the game leaving by itself).
  The match ends for both at once: status `abandoned`, `left` / `left_side`
  naming that player. A player who sent no report and is gone from the server
  while the other's game lost them (`disconnected`) left it too. Optional (0.1.14),
  how the game ended, which the server logs with the match: `exit`, the
  launcher's word for it from the game's log (`crash`, `asked to leave N`,
  `shut down`, `relaunched`, `window closed`, `ended from outside`; up to 40
  characters), and `exit_code`, the process's exit code; a value it does not
  take is dropped. A ranked set is lost by the one who left, whatever its
  score (1.9.1); a Casual series offers the rematch again, the match counting
  for nobody. Two games that lost each other with neither at fault
  (`no_contest` with `disconnected` or `error`, `unconfirmed`) end a ranked
  set with no result; a Casual series goes on the same way. A desync does not
  end a series.
- Lobby matches count as casual. After any final status the lobby returns to
  `open` and both players' `ready` flags are cleared.

### 1.9.1 Series: casual rematches, ranked sets

After a queue match (once its `match_result` went out) the two players are in
a series. Casual: another match whenever both want one, with the same fighters
or after both pick again, without end (0.1.14: matches that did not count
included); it ends only when a player leaves it or lets the time run out. Ranked: a set, won by the first to win 3 matches
(`need`; 2 before 0.1.21), each match's winner keeping their fighter for the next one; Elo moves once, when it ends,
and so do the two ranked records: a set won and a set lost, whatever its score (1.0.1; each match before). A
player is in one series at a time; searching again or creating/joining a lobby leaves it.

```json
S: {"type":"series","series":"8d01c3e2a9f4","game":"sc5","mode":"versus","ranked":true,
    "state":"between","need":3,"played":1,"opponent":"Kilik","you":1,"them":0,
    "you_ready":false,"them_ready":false,"you_choice":"","them_choice":"",
    "same_fighters":true,"seconds_left":90,"keeps":"Taki","opponent_elo":1500,
    "opponent_title":"Squire"}
C: {"type":"series_ready","series":"8d01c3e2a9f4","ready":true,"choice":"rematch"}
S: {"type":"series_ack","series":"8d01c3e2a9f4"}
C: {"type":"series_ready","series":"8d01c3e2a9f4","ready":false}
S: {"type":"series_left","series":"8d01c3e2a9f4"}
S: {"type":"series","series":"8d01c3e2a9f4", ...,"state":"over","outcome":"won","winner":"Taki",
    "left":null,"elo":{"Taki":{"before":1500,"after":1516,"delta":16,"title_before":"Squire",
                                 "title_after":"Squire"},
                       "Kilik":{"before":1500,"after":1484,"delta":-16,"title_before":"Squire",
                                "title_after":"Squire"}}}
```

- `state`: `between` (the server waits for both players' `series_ready`),
  `playing` (a match of it runs), `over`. `you`/`them` count each player's
  agreed match wins in the series; `seconds_left` is the time left between
  matches: 60 s for a casual rematch, 90 s for a set's next match (the last
  match's ending plays first). The server sends the state to both players at
  every change, and to a player who logs in again in the middle of one.
- A launcher sends `series_ready` true for a ranked set by itself, for a casual
  rematch when the player chose one: with `"in_place":true` while its game
  still waits at the last match's result screen, connected to the other game
  (SCV 0.1.2: a queue match that ended there, against a game reached straight,
  not through the relay or the tunnel), else once its game is back at its hold
  point. When both are ready the server starts the next match like a lobby
  match: `match_found` with `accept_required` false, `"series"` and the same
  sides, then the usual setup (1.8). When both said `in_place` and the last
  match went straight between them, `match_found` says `"in_place":true` and
  `match_go` follows at once on the last match's route (its `remote` and
  `rtt_ms`, a new `match` and `seed`): no UDP hello, no punch; the launcher
  sends the game `rematch` (3.3) instead of `start`. A launcher whose game waits
  at its result screen and gets a `match_found` without it parks the game first
  (it lets go of the game port at once) and goes the usual way.
- In `versus` (the server's `SELECT_SCREEN_MODES`) the next match's picks: in a
  ranked set (0.1.21) only the last counted match's loser picks again on the
  game's select screen: its winner keeps the fighter both reports named for
  them (1.9 `p1_char`/`p2_char`, with its costume), which `match_go` gives
  their side (`"p1":"12.1","p2":"-"`) and the autopilot picks in both games;
  `keeps` names that player (`null`: both pick, as after a win without
  fighters both reports agreed on). Casual (SCV 0.1.2), `choice`: `rematch`
  from both plays the last match's fighters again (`match_go` `p1`/`p2` are the
  game's character ids from both results, on the same stage when it goes on
  from the result screens: the game's own Rematch), `select` from either sends
  both back to the select screen (`p1`/`p2` `-`), and so does a rematch after a
  match whose fighters the reports did not agree on (`same_fighters` false). A
  match that did not count (a desync) is played again the same way.
  `opponent_elo` and `opponent_title`: the other player's rating as they
  searched, in ranked (`null` in casual).
- `choice` (casual, other modes; default `rematch`): `rematch` plays the last
  match's fighters again when both choose it (`match_go` `p1`/`p2` are the
  game's character ids from both results, the autopilot picks them); `select`
  from either player sends both to the select screen (`p1`/`p2` `-`). Without
  fighters both reports agreed on (`same_fighters` false: a desync, a mode
  without fighters, an older launcher) a rematch uses the characters both
  queued with. `you_choice`/`them_choice` show what each chose once ready. A
  ranked set of another mode keeps the characters both queued with.
- Once a series is over, a launcher searches again by itself with the search
  it came from (`cfg::searchAgain`, on unless the player turned it off), unless
  the player left the series themselves, let a Casual rematch offer run out
  without taking it, or neither came back to a ranked set.
- `over` carries `outcome`:
  - `won`: a set's `winner` reached `need` wins. Elo update with `K = 32`,
    `E_a = 1 / (1 + 10^((R_b - R_a) / 400))`, `d = round(K * (S_a - E_a))`
    (half away from zero), `R_a' = R_a + d`, `R_b' = R_b - d` (zero-sum,
    integer ratings); `elo` lists both players' change, with the title of the
    rating before and after (`title_before`, `title_after`).
  - `forfeit`: `left` left the set after at least one of its matches counted:
    `series_ready` false, its time ran out while the other was ready, the next
    match was cancelled on their side, or they searched or went to a lobby. They
    lose the set (Elo as for `won`, the other player winning), whatever the
    score: one match won and gone gains nothing.
  - `void`: a set where nothing counted yet was left, its two games lost each
    other, or 3 matches in a row did not count; no rating change.
  - `left` (casual: a player declined or went) and `timeout` (casual: no
    rematch in time; ranked: neither came back).
- A launcher that drops between two matches has 20 s to log in again before
  its time runs out. Ranked sets are logged in the `sets` table (outcome,
  winner, score, both ratings before and after).

### 1.10 Lobbies

A lobby has a 5-character code from `ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, at
most two players and eight spectators, an optional password, and is always
casual in v1.

```json
C: {"type":"lobby_create","name":"Friday sets","game":"sc5","mode":"versus",
    "password":"","rounds":3,"region":"eu","character":"Siegfried","death_traps":true,"koth":false}
C: {"type":"lobby_list","game":"sc5"}                          (game optional)
S: {"type":"lobbies","lobbies":[{"id":"K7WQ2","name":"Friday sets","host":"Taki",
    "game":"sc5","mode":"versus","region":"eu","players":1,"max_players":2,
    "spectators":0,"max_spectators":8,"locked":false,"state":"open","rounds":3,
    "death_traps":true,"koth":false,"in_line":0}]}
C: {"type":"lobby_join","lobby":"K7WQ2","password":"","character":"Ivy","spectate":false}
C: {"type":"lobby_role","role":"spectator"}                    (or "player")
C: {"type":"lobby_leave"}
C: {"type":"lobby_chat","text":"gg"}
C: {"type":"lobby_character","character":"Ivy"}
C: {"type":"lobby_ready","ready":true,"delay":2,"relay":false,"hide_always":false,"link":"wifi","server_rtt":31}
                                           (delay optional, 0-4, delay_auto 1.7; relay, link, server_rtt 0.1.8;
                                            "in_place":true,"choice":"rematch"|"select" SCV 0.1.4: below)
C: {"type":"lobby_start"}                       (host only: at once; launchers from 0.1.11 never send it)
C: {"type":"lobby_settings","death_traps":false}               (host only, 0.1.8; SCV launchers never send it)
S: {"type":"lobby","lobby":{"id":"K7WQ2","name":"Friday sets","host":"Taki","game":"sc5",
    "mode":"versus","region":"eu","rounds":3,"locked":false,"state":"open","match":null,"starts_in":2.4,
    "death_traps":true,"max_spectators":8,"koth":false,
    "members":[{"name":"Taki","character":"Siegfried","ready":true,"host":true,"spectator":false,
                "title":"Grandmaster"},
               {"name":"Kilik","character":"Ivy","ready":true,"host":false,"spectator":false,
                "title":"Squire"},
               {"name":"Kitana","character":"","ready":false,"host":false,"spectator":true,
                "title":"Wanderer"}],
    "chat":[{"from":"Kilik","text":"gg","ts":1761300042.1}]}}
S: {"type":"lobby_chat","lobby":"K7WQ2","from":"Kilik","text":"gg","ts":1761300042.1}
S: {"type":"lobby_left","lobby":"K7WQ2"}
```

- `lobby_create`, `lobby_join` and `lobby_ready` may carry `link` and
  `server_rtt` (as in `queue_join`); the lobby state's members show them as
  `link` and `ping` (`null` unknown).
- **The loser chooses the next match** (SCV 0.1.4; a `versus` lobby, not King
  of the Hill): a lobby match's `match_found` says `"result_choice":true`, and
  a match its two games played straight (no relay, no tunnel) ends with both
  games at their result screens, where the match's loser (player 1 after a
  draw) chooses REMATCH or CHARACTER SELECT with their own controller
  (section 3.3, `choose=1`). Each launcher then sends `lobby_ready` with
  `"in_place":true` and that `"choice"` (`rematch` or `select`); once both
  players are ready in place the next match starts at once, without the
  countdown, from where the two games wait: `match_found` with
  `"in_place":true`, then `match_go` on the last match's route (its `remote`
  and `rtt_ms`, a new `match` and `seed`) and the game's `rematch` (as a
  series' next match in place, 1.9.1). `rematch` plays the fighters both
  games' results named again (`match_go` `p1`/`p2` the game's ids, the same
  stage: the game's own Rematch); `select` (or fighters the two results did
  not agree on) sends both back to the select screen (`-`). A player who
  readies in the lobby instead (or one launcher that is not at the result
  screen) takes both the usual way, `in_place` false, its game parked first.
  Nobody choosing in a minute parks both games (the players ready again).
- Each member's `title`: their ranked title in the lobby's game and mode
  (someone who never played it ranked: the starting one; 1.3 `ranks`), whose
  badge the launcher shows by their name.
- Each member's `version` and `build` (SCV 0.1.4): their launcher's, as its
  `hello` said (`build` `null` for none, 1.3). Two players of different
  versions run different games, which cannot play each other: the launcher
  says so in large type before anyone readies, names who has the older one
  and must update, and keeps its Ready off against such a player; the same
  version in two builds (a test build) is said too, Ready left on.
- A member whose connection drops (or is replaced by a new login of the same
  account) keeps their place for 60 s: when that account logs in again within
  it, the server puts it back in the lobby (a player's place if one is free,
  else a spectator's) and pushes the `lobby` state as after a join. Leaving
  with `lobby_leave` or `logout` gives the place up at once.
- The full `lobby` state is pushed to every member after any change; `chat`
  holds the last 30 messages. Chat text: 1-200 characters, at most one
  message per 0.5 s per player (`slow_down`).
- `lobby_create` and `lobby_join` reply with `lobby`; errors: `busy` (searching,
  or a match of this player's set up or running), `no_such_lobby`, `lobby_full`,
  `bad_lobby_password`. A session is in one lobby at most (SCV 0.1.4): creating or
  joining another one, once the server has checked it lets the player in, leaves the
  one they are in, as `lobby_leave` would, and says so first with
  `{"type":"lobby_left","lobby":"<the old one>","joined":"<the new one>"}` (until
  then `busy`); a request turned away leaves them where they were, and joining the
  lobby they are in replies with its state. `rounds` is 1, 3 or 5, the best of (the launcher names them
  1, 2 or 3 rounds to win, 3 by default); matchmaking's matches are best of 5, first to 3 as the console's Battle Count.
- Two players both ready start the lobby's match by themselves (0.1.11): the
  server counts down 3 s (`starts_in`, the seconds left, in every `lobby` state
  meanwhile; `null` otherwise), and anything that clears a `ready` (a player
  un-readying, a new character, a role or settings change) or takes a player
  away calls it off. Then the match flow of 1.8 starts at step 3 with the host
  as side 1 (when the host plays); the lobby `state` becomes `in_match` with
  `match` set. `lobby_start` (host only, `not_host`; two players both ready,
  `not_ready`) starts it at once, its reply that `lobby` state. Changing character clears
  your `ready` flag. `lobby_settings` changes the lobby's `death_traps` (MK3D's, 1.7) between matches (host
  only, `not_host`; not while a match is set up or runs, `busy`), which clears every member's `ready`.
- Spectators: `lobby_join` with `spectate:true` joins to watch (also while a
  match runs, and when both places to play are taken: a plain join then gets
  `lobby_full`). `lobby_role` moves a member between the two: to `player`
  needs a free place and no match running (`lobby_full`, `busy`), to
  `spectator` is refused to a player whose match is set up or runs (`busy`).
  Spectators neither ready (`not_player`) nor play. `lobby_character` is
  refused to a player whose match is set up or runs (`busy`); a spectator may
  change theirs meanwhile (King of the Hill: for their turn).
- If the host leaves, a player (else another member) becomes host, and a
  lobby named "<old host>'s lobby" (the launcher's default) is renamed
  "<new host>'s lobby" (0.1.11); an empty lobby is deleted. A member who disconnects leaves the lobby (and a player
  cancels a match that has not reached `playing`; a spectator does not).
- States: `open`, `full` (both places to play taken), `in_match`. `locked` =
  has a password.
- Lobby-only modes (1.0.7: Online Practice, `sparring`, every game): played in
  a lobby a friend joins by its code alone. `queue_join` refuses them
  (`bad_mode`), `lobby_list` leaves them out, and the lobby is for its two
  players: no spectators (`max_spectators` 0; a third `lobby_join`, watching
  or not, gets `lobby_full`, and `lobby_role` to `spectator` `bad_request`).
  Both players pick on the game's own select screen (their `character` is
  always empty), and the players stay ready after
  each match, so the next one counts down by itself: a player who presses
  BACK in the game goes back to the select screen with the other. It has no
  King of the Hill (`koth` with it: `bad_mode`).
- `koth`: a King of the Hill lobby (1.10.2), chosen with `lobby_create` and
  kept for the lobby's life; the list's `in_line` counts the spectators who
  wait in line to play (also counted in `spectators`).

### 1.10.1 Spectators

Each player's game writes what a spectator's game needs to play the match
itself to its launcher (section 3.3, `feed`); the launchers pass it on to the
server, which keeps it for the match and passes it to the lobby's spectators,
whose launchers hand it to their games. Nothing of it goes between the two
players.

```json
C: {"type":"feed","match":"5f2c9a01d4e3","data":"<base64>"}       (a player's launcher, every 0.1 s)
S: {"type":"spectate","match":"5f2c9a01d4e3","lobby":"K7WQ2","game":"sc5","mode":"versus",
    "seed":2718281828,"rounds":3,"death_traps":true,"p1":"Siegfried","p2":"-",
    "p1_name":"Taki","p2_name":"Kilik","p1_title":"Wanderer","p2_title":"Squire"}
S: {"type":"spectate_feed","match":"5f2c9a01d4e3","data":"<base64>"}
S: {"type":"spectate_end","match":"5f2c9a01d4e3","status":"agreed","winner":1,
    "p1_rounds":2,"p2_rounds":0}
```

- `data` is whole packets of the game's spectator feed, each after its
  little-endian `u16` length, at most ~9 KB a message. The packets are the
  netplay's (header `SC5R`, type, side, barrier number): `5` the player's pick
  on the select screen, `6` the game's READY words at the meeting point and
  its seed, `7` the player's own inputs (`u32 first, u8 n, n x (u32 buttons,
  u32 sticks)`), `8` the game's state checks, `9` the frame it left the match
  at. A player's launcher may send only its own side's packets
  (`bad_request` otherwise); feed for a match that is gone is dropped
  silently. A match's feed holds at most 8 MB.
- `spectate` goes to every spectator when a lobby match reaches `match_go`,
  and to a spectator who comes in while one runs, followed by everything the
  feed holds so far (batched, paced by the spectator's connection);
  `spectate_feed` then passes on each `feed` message as it comes.
- `spectate_end` goes out with the match's result (1.9). The feed is still
  passed on for 180 s after it: the match's end plays after the result, and
  spectators run behind the players.
- A spectator's game starts like a player's (`start` with `watch=1`, section
  3.3), takes both picks from the feed, meets both games' READY words where a
  player's game meets the other's, and plays every frame from both players'
  inputs, never predicting: a frame waits for both inputs, and once they ran
  out for 20 more. A game far behind (a spectator who came in late, 180
  frames or more) runs every frame unseen and unheard, as fast as its PC
  runs them, until it is within 30 frames: the match comes on where the
  players are (0.1.18; before, three frames of four unseen, a fast-forward).
  Its own checks are compared with the players'.

### 1.10.2 King of the Hill

A lobby created with `"koth":true`: two players fight, the winner stays on as
the king and plays the next match as side 1, the loser goes to the back of the
line and the first in line plays the king; when the king loses, the player who
beat them is the new king. Everyone else in the lobby watches, as a spectator
(1.10.1). After each match its spectators rate how the winner played, a whole
number from 1 to 10 each, and every rating adds that many Respect Points to the
winner's, in the lobby's game and mode. The matches are casual as every lobby
match (the Casual record moves, Elo does not).

```json
S: {"type":"lobby","lobby":{"id":"K7WQ2", ..., "koth":true,"king":"Kilik","streak":2,
    "rating":{"match":"5f2c9a01d4e3","winner":"Kilik","loser":"Taki","seconds":31.5,
              "raters":["Kitana","Sindel"],"rated":["Kitana"]},
    "ready_by":82.0,
    "members":[{"name":"Taki","spectator":true,"line":2,"respect":40, ...},
               {"name":"Kilik","spectator":false,"line":null,"respect":112, ...},
               {"name":"Kitana","spectator":false,"line":null,"respect":0, ...},
               {"name":"Sindel","spectator":true,"line":1,"respect":7, ...}], ...}}
C: {"type":"lobby_rate","match":"5f2c9a01d4e3","score":8}
S: {"type":"lobby_rated","match":"5f2c9a01d4e3","score":8,"winner":"Kilik"}
S: {"type":"lobby_chat","lobby":"K7WQ2","from":"","text":"Kilik gets 15 Respect Points from 2 ratings (127 in all).",
    "ts":1761300112.4}
```

- The line: a `lobby_join` to play (`spectate:false`) while both places are
  taken or a match runs joins as a spectator at the end of the line; with a
  place free and nobody waiting it plays at once, as in any lobby. Each
  member's `line` is their place in it (1: next), `null` for the two players and
  for spectators who watch only (`lobby_join` with `spectate:true`).
  `lobby_role` `player` gets a spectator in line (playing at once when a place
  is free and nobody waits), `spectator` takes them out of it to watch only, or
  steps a player aside (not while their match is set up or runs, `busy`): the
  first in line takes the free place. A player who leaves (or whose connection
  drops) gives their place to the first in line at once, a king their crown;
  one back within the 60 s of 1.10 is put at the end of the line.
- After a match with a winner (1.9 `agreed`): `king` is the winner (`streak`
  their wins in a row), side 1 in the next match (with no king, the host when
  they play, as in any lobby). With someone in line the loser becomes a
  spectator at its end and the first in line plays; with nobody waiting the
  two play again. A match one player's game left (`abandoned`) is lost by that
  player and rated by nobody; one nobody won (a draw, a desync, a lost
  connection, a dispute) has the same two play again (after a draw the lobby
  says `A draw: <p1> and <p2> play again.`). The lobby says what happened in its
  chat, in lines with an empty `from`: who is king, who is next, Respect Points.
- `rating`: open from the result of a match with a winner that had spectators,
  for 40 s (`seconds` left) or until every spectator of it still in the lobby
  has rated; `raters` are the lobby's spectators when the match ended, `rated`
  those who have. `lobby_rate` (`score` a whole number 1-10, else
  `bad_request`) counts once: `not_rater` (the two players, or someone who did
  not watch it), `already_rated`, `rating_closed` (another match, or its time
  is up). Each rating is kept (table `ratings`) and added to the winner's
  Respect Points (table `respect`, points and how many ratings) as it comes;
  when the rating closes the lobby's chat says the sum. The next match's 3-2-1
  countdown does not start while a rating is open (a host's `lobby_start`
  closes it).
- `ready_by`: with someone in line and no match, the seconds the two players
  have left to ready (90 from when they became the two); one who has not by
  then goes to the back of the line (a king loses the crown) and the first in
  line plays instead. `null` when nobody waits.
- A launcher without `koth` in its `hello` (1.3) may only watch such a lobby:
  creating one, joining it to play and `lobby_role` `player` get
  `update_needed`.

### 1.11 Leaderboard

```json
C: {"type":"leaderboard","game":"sc5","mode":"versus","limit":100,"offset":0,"by":"elo"}
S: {"type":"leaderboard","game":"sc5","mode":"versus","by":"elo","total":57,
    "rows":[{"rank":1,"name":"Taki","elo":1712,"title":"Master","wins":40,"losses":12,"respect":40}],
    "me":{"rank":9,"elo":1580,"title":"Warrior","wins":8,"losses":3,"respect":0}}
C: {"type":"leaderboard","game":"sc5","mode":"versus","by":"respect"}
S: {"type":"leaderboard","game":"sc5","mode":"versus","by":"respect","total":12,
    "rows":[{"rank":1,"name":"Kilik","respect":112,"ratings":14,"elo":1600,"title":"Demi God",
             "wins":9,"losses":4}],
    "me":{"rank":5,"name":"Sindel","respect":7,"ratings":1,"elo":475,"title":"Beginner","wins":0,"losses":0}}
```

`by` (default `elo`; else `bad_request`): `elo` lists the accounts with at
least one ranked game in that game and mode, ordered by Elo, then wins, then
name, each with the title of their Elo (1.3 `ranks`); `respect` lists every
account with Respect Points there (1.10.2), the most first, then the fewer
`ratings` that gave them, then name, each with their ranked record (or the
start of one: Elo 475, 0-0). Every row and `me` carry the player's `respect`
there. `limit` at most 200. `me` is present when the asking account is on that
board.

### 1.12 Keepalive and disconnects

```json
C: {"type":"ping"}      S: {"type":"pong","t":1761300050.0}
```

Closing the connection leaves queues and lobbies, cancels matches that have
not reached `playing` (`peer_left` for the other side, who is requeued when
the match came from the queue), and keeps `playing` matches open for the
other side's result report.

### 1.13 Crash reports (0.1.12)

```json
C: {"type":"crash_report","game":"sc5","code":"C0000005",
    "where":"sc5.exe+1A2B3C on a game thread","during":"Playing"}
S: {"type":"crash_ack"}
```

A launcher whose game crashed says so: the game, the exit code (hex), where
(the game log's crash line after its code: a module and an offset, and the
thread; from 0.1.14 an abort, code `C0000409`, also lists in brackets the calls
that led to it: `sc5.exe+1459A4 (sc5.exe+147ABC sc5.exe+5F21C) on a game
thread`) and what was going on (the match phase, `single player`, `practice`,
`preparing`, `local test`). The server logs it with the player's name
(`CRASH <name> <game> (launcher <version>) during <what>: <code> <where>`) and
keeps nothing; plain printable text only (`bad_request`), at most 20 a session
(more are dropped without a word).

### 1.13.1 Desync reports (0.1.15)

```json
C: {"type":"desync_report","game":"sc5","match":"7658cd7bfcd7","frame":30,
    "lines":["[    0.004] CPU: AMD Ryzen 7 9800X3D 8-Core Processor (16 threads)", "...",
             "[   28.425] netplay: stage 4, costumes 1 and 0",
             "[   29.000] error: netplay: DESYNC at frame 30 (ours 1234ABCD/00000001, theirs 5678EF00/00000001)",
             "Game files: 587 files, 4050117929 bytes, names and sizes 3017906901d6341d"]}
S: {"type":"desync_ack"}
```

A launcher whose match went out of step (its game's `desync` status, after the
`result` with outcome `desync`) sends what its game logged of it: from the log's
start, Windows, the processor and the game's executable (the log's `Game:` line, `default.xex` and its
SHA-1); from the match's start (the link's `start` or `rematch` line, and the
`command` line the game logs before it), the lines below; last, the game's
extracted files: how many, how large, and a hash of their names and sizes (a
modded or reduced copy of the disc differs there), and (SCV 0.1.2) the PC:
`This PC: <processor>, <n> threads, Windows <kernel version>, game code for
<x86-64-v3 or v2>` (what build-game.cmd compiled the game's code for there).
SCV's game logs its netplay as `sc5 net:` (the failed check: `DESYNC at frame
<f>: <hash> here, <hash> there`), its setup as `sc5 versus:`, its picks as
`autopilot: side <n> picked`; from SCV 0.1.4 it writes detail lines right after
the failed check, each with `sc5 desync:` (up to about 80, at most 200
characters each). Of the match's lines the report keeps (SCV 0.1.4):

- every `sc5 desync:` line (100 at most);
- the setup: the start, the picks, the stage, the meeting, the words both games
  agreed on, the blocks of the fight (40 at most);
- of the statistics the game logs every 10 s (`... rollbacks (...`, `keeping a
  frame top ...`), only the last report before the failed check;
- the last 30 other lines before the failed check (round ends, ring outs, the
  game's own errors and warnings, not the graphics', which repeat by the
  thousand), the failed check itself, and up to 10 lines after it;

in the log's order, 200 lines and about 56,000 characters at most (past that
the other lines go first, the oldest first, then the detail from its last).
Until 0.1.4 it kept 36 lines, the statistics among them (MK3D Online's filter,
`netplay: frame <n>:`, never matched SCV's `sc5 net: frame <n>: ... rollbacks`),
so in a long match the statistics filled it and the lines just before the check
(the round's end, a ring out) were cut. IP addresses and user folders are taken
out.

The server logs each line with the player's name (`DESYNC_REPORT <name> |
<line>`, after a line naming the game, launcher version, match and frame), so
both players' reports of a match stand side by side, and keeps nothing: at most
200 lines (40 until SCV 0.1.4) of 200 printable characters (else
`bad_request`), 10 reports a session. A `desync_report` alone may be a line of
up to 64 KB (`MAX_DESYNC_LINE`; every other message still 16 KB, 1.1, else
`too_long` and the connection closed). Once both players' reports of a match
are in (two accounts, the same `match`, within 10 minutes), the server logs one
summary line, so a night's desyncs read at a glance from the journal:

```
DESYNC_PAIR 7658cd7bfcd7 Taki at frame 41730, Kilik at frame 41730; stage 17; Siegfried vs Mitsurugi (launchers 0.1.4 and 0.1.4)
```

From SCV 0.1.4 a failed check does not end the match when the two games can play
the round again from its start (GAME.md, "The round again"): the game's status
then says `in_match match=<id> again=<times> again_frame=<the failed check>`
(3.4), and for each time the launcher sends the same report with `"again":
true` and that frame, while the match goes on. The server's lines say so
(`... at frame 930, the round played again: 40 lines`, and the pair's `; the
round was played again and the match went on`); `again` must be a boolean.

with each report's frame (`(the frames differ)` when they do), the stage as the
game logs it (its `sc5 versus:` line) and the two fighters (the picks, else the
start line's `p1`/`p2`), each pair of them followed by `(they differ)` when the
two reports disagree (`?` for what a report's lines leave out).

From 0.1.24 the failed check's lines are followed by the blocks it covered as
short hashes (`netplay: frame 3300 block 1 at 002E9704, pieces 0-29: ...`: each
block's 64-byte pieces, 16 bits a piece, pointers left out as the check leaves
them): the pieces that differ between the two players' reports are where the
games parted (MK3D Online's game; SCV's detail is its `sc5 desync:` lines).

From 1.0.2 a game checks every 30th frame once both players' real inputs before
it are in (before, only a frame that happened to run on real inputs alone was
checked, so at a high ping two games that had parted could play on for seconds),
and after a failed check the two games swap their records of it over the
netplay connection (DETAIL packets, the game's netplay in `sc5/`): each
frame's state hash since the check before, then the piece hashes of the first
frame whose state differs. Each game then logs `netplay: desync detail: ...`
lines: the first frame the two states differ at, both players' inputs of the
four frames before it, what each character did then (as the game logs it), this PC's MXCSR (the emulated
math follows the host's) and display, and `netplay: differs at <frame>, block <n> +<offset>:
<16 words>` for every piece the other game's hashes disagree with (12 at most),
so the server's log holds both players' words side by side. The desync status
waits for that, 2 s at most; with an older game on the other side the piece
hashes come as before (MK3D Online's game). The game files' line adds `contents
<hash>`, every file's bytes hashed once in the background
(`game-files-<game>.txt` beside the launcher keeps each file's hash, size and
time), or `contents not hashed yet`.

### 1.13.2 F3 reports (0.1.18)

```json
C: {"type":"report_file","report":"0123456789abcdef","game":"sc5","name":"final.jpg",
    "part":0,"parts":3,"data":"<base64>"}
C: {"type":"report_file","report":"0123456789abcdef","game":"sc5","name":"frame.txt",
    "part":0,"parts":1,"data":"<base64>","zlib":true}
C: {"type":"report_done","report":"0123456789abcdef"}
S: {"type":"report_saved","report":"0123456789abcdef","files":25}
```

A player who sees something drawn wrong presses F3 in the game: the runtime
saves what it drew (the frame's draws `frame.txt`, its picture `final.bmp`,
the next 20 frames' pictures `next_00.bmp` .. `next_19.bmp` and the textures it
used `tex_*.bmp`) under the session's `captures` folder, and the game shows a
line saying so. The launcher (`ReportSender`) finds the new capture in a running
game's session once its last picture is saved (or 15 s on), and sends it: the
pictures as JPEG, the textures as PNG (smallest first, 4 MB at most), the draws,
the end of the game's log (256 KB, IP addresses and user folders taken out)
and `info.txt` (launcher version, game, session, display settings) compressed
(`zlib`: as Qt's qCompress makes it, a big-endian length then zlib). Each file
goes in parts of at most 16,000 base64 characters, in order, two parts every
0.1 s so the match's own messages pass; `report_done` ends it. The server
writes each file once its last part is in, into
`<reports_dir>/<day>/<time>-<player>-<game>-<report>` (`reports` beside the
database), and logs `REPORT <name> <game> (launcher <version>): <n> files,
<KB> KB, in <folder>`. Limits: 3 reports a connection, 12 MB and 160 files a
report, names `[A-Za-z0-9_-]{1,40}` with `.jpg`, `.png`, `.txt` or `.log`, 4 GB
of reports on disk (`too_many`, `too_large`, `unavailable`, `bad_request`).

---

## 2. NAT traversal: UDP rendezvous

### 2.1 Server endpoint

The server listens on one UDP port (default **7778**) next to its TCP port.
Datagrams are single JSON objects, UTF-8, at most 512 bytes. The server never
answers a datagram that does not carry a valid token, so it cannot be used
as a reflector, and it never answers with more bytes than it received
(clients always include `lan` and `t`, which keeps the hello larger than the
answer).

### 2.2 Punch hello

Sent by the launcher **from the game UDP port** (the port the game binds
later, default 7001), so the NAT mapping the server observes is the one the
game will use:

```json
L->S: {"type":"hello","token":"<udp_token>","match":"5f2c9a01d4e3","lan":"192.168.1.20:7001","t":123456,"rtt":24}
S->L: {"type":"hello_ack","public":"203.0.113.7:7001","t":123456}
```

- `token` is `account.udp_token` of this connection; `match` is optional
  (empty while queued). `lan` is the launcher's LAN address (the local address
  of its TCP connection) with the game port. `t` is echoed so the client can
  measure its round trip to the server; the client sends it in later
  `queue_join` messages as `server_rtt`, and as `rtt` (optional, 0.1.8) in
  its next hellos, which update a waiting search's `server_rtt` (1.7).
- The server records the packet's source address as the client's **public
  endpoint** and the `lan` field as its **LAN endpoint**.
- Clients send a hello when they join a queue, every 15 s while queued (keeps
  the NAT mapping and the round-trip figure fresh), and every 250 ms after
  `match_found` until `hello_ack` arrives (giving up after 4 s: then the
  match goes through the tunnel, 2.4.1).
- The launcher binds the game port on all interfaces, so a player across the
  internet reaches it even when the server runs on the same PC; one-PC tests
  set `[dev] udp_loopback=true` to stay on 127.0.0.1.
- A peer endpoint at a loopback address, or a private one while this launcher
  reaches the server at a public address, belongs to a player on the server's
  own network: the launcher punches it at the server's address with the
  peer's port instead.
- `match_peer` (1.8 step 3) carries the other side's `public` and `lan`
  endpoints. `same_nat:true` means both public IPs are equal; then the LAN
  endpoint is the one expected to work (many routers do not hairpin). Only
  then is `lan` sent at all.

### 2.3 Peer punch and ping

Both launchers, still on the game port, send to **both** candidate endpoints
of the peer every 100 ms until one answers, for at most `punch_timeout`
seconds:

```json
{"type":"punch","match":"5f2c9a01d4e3","key":"<32 hex>","side":1}
{"type":"punch_ack","match":"5f2c9a01d4e3","key":"<32 hex>","side":2}
```

- Packets whose `match` or `key` do not match the current `match_peer` are
  ignored. A `punch` is always answered with `punch_ack` to its source
  address. The first candidate that answers with `punch_ack` is the working
  endpoint (`via` = `lan` or `public`). A `punch` from an address that is not
  a candidate (the peer's router chose another public port) becomes a
  candidate too.
- Then 5 pings, 100 ms apart, to the working endpoint:
  `{"type":"ping","match":"...","key":"...","seq":n,"t":<ms>}`, answered with
  `{"type":"pong",...}` carrying the same `seq` and `t`. `rtt_ms` = median of
  the answered pings; at least 3 must come back.
- The launcher keeps answering the peer's punches and pings until it gets
  `match_go` or `match_cancelled`.
- The result goes to the server as `ping_report` (1.8 step 4); `ok` is
  `false` when no candidate answered or fewer than 3 pings came back.
- After `match_go` the launcher closes its socket and writes the `start` line
  at once; the game binds the same port and should start sending within a
  second, so the NAT mappings opened by the punch stay alive.

### 2.4 Relay

When the direct punch fails, when a player hides their address (`relay`,
1.7), or when the server runs with `--relay-only`, the server's UDP port
carries the match. The server joins the two endpoints their
hellos came from; from then on every datagram from one of them that is not a
`hello` goes to the other one byte for byte: the launchers' `punch` / `ping`
JSON while measuring, then the games' packets. Each launcher punches the
server's UDP endpoint (the one its hellos go to, the only candidate,
`via:"relay"`), and after `match_go` its game sends to it from the same port,
so the router keeps the mapping the hellos opened.

- Only the two endpoints of a match in `punching` or `playing` are joined; a
  packet is never answered with more bytes than it had; each endpoint may send
  `relay_rate` packets a second (400, bursts of 800); a datagram over 1472
  bytes is dropped.
- The route ends 60 s after the match is done or cancelled: the games report
  the result at the round that decides the match and play its end (the
  winner's pose) through the relay after it, for up to 30 s before their
  launchers park them (it used to end after 15 s, which cut such an end off).
- The server logs each relayed match and the packets that went each way.
- `match_peer` with `relay` says why (0.1.18): `relay_why` is `hidden` (a
  player hides their address; `relay_mine` true for that player), `tunnel`,
  `unreachable` (the punch failed) or `relay_only`. The launchers tell their
  players when a hidden address is why: two players far from the server (in
  Virginia) get far more ping through it than directly (two in the UK about
  180 ms instead of about 25). Since 0.1.17 almost every relayed match was
  relayed for that reason (705 of 736), not for a failed punch (29).
- A match relayed only for a hidden address whose ping through the relay is
  over `direct_above_ms` (0.1.18, `--direct-above-ms`, 100 by default) tries a
  direct connection once (1.8, step 5); the relay's routes linger meanwhile,
  and a way back (`"back":true` in the relay's `match_peer`) makes them again.

### 2.4.1 Tunnel (0.1.12)

A PC whose UDP never reaches the server (a firewall, a VPN or a network that
blocks it: its hellos get no `hello_ack`) plays through the relay over its TCP
connection instead:

```json
C: {"type":"udp_tunnel","match":"5f2c9a01d4e3"}
S: {"type":"match_peer","match":"5f2c9a01d4e3","side":1,"key":"<32 hex>","punch_timeout":10,
    "same_nat":false,"relay":true,"tunnel":true,"peer":{"public":"","lan":""},"opponent":{...}}
C: {"type":"tunnel","data":"<base64 of one datagram>"}
S: {"type":"tunnel","data":"<base64 of one datagram>"}
```

- The launcher asks when the match's hellos got no answer within 4 s, or at
  once while its last hello went unanswered (a search's, a match's); a hello
  answered again ends that. Only before `match_peer`: later the server keeps
  the way it chose. The match then goes through the relay (2.4) with this
  player's end on the connection: their `match_peer` says `"tunnel":true`
  (the other player's `false`, a plain relayed match for them).
- The launcher opens a UDP port on 127.0.0.1 that stands in for the relay
  (`launcher/src/TunnelBridge.cpp`): every datagram the game port sends to it
  goes to the server as `tunnel`, and every `tunnel` from the server goes to
  the game port from it. The launcher punches and pings it as the relay's
  endpoint (`via:"tunnel"`, `remote` = that port), so `match_go` gives the
  game that port as its peer, and the game, at a loopback peer, binds
  127.0.0.1.
- The server treats a `tunnel` as a datagram from that player's end of the
  relay: the same route, rate and size limits; what goes to a tunnelled player
  goes as `tunnel` on their connection (by account: a connection that drops
  and comes back gets the rest). `tunnel` is never answered: one without a
  route (the match is over) or with bad data is dropped.
- TCP carries every packet in order, so a lost segment holds the ones after it
  until it is sent again: a tunnelled match has the odd hitch where UDP would
  just lose a packet, and rollback hides most of it. The launcher's connection
  sends at once (`TCP_NODELAY`).

### 2.5 Port mapping

The launcher opens the game UDP port on the router (UPnP Internet Gateway
Device: SSDP discovery, the device description, `AddPortMapping` with a 1 h
lease, renewed every 20 min, permanent on routers that refuse leases) unless
Settings turns it off (`online/upnp`). A router whose own address is private
or carrier-grade NAT (100.64/10) cannot be opened from outside; the launcher
says so and leaves the match to punching and the relay. The server does the
same for its TCP and UDP ports with `--upnp` (a server on a home PC).

### 2.6 Out of scope

- NAT-PMP / PCP port mapping (UPnP only).
- IPv6: the server and launcher use IPv4 only.

---

## 3. Launcher <-> game process

The launcher owns the game process. There is no socket between them, only environment variables and two small text
files in a per-session folder (and, for a lobby match, the spectator feed's TCP port on 127.0.0.1, 3.6). This is
MK3D Online's contract with the `SC5_` prefix in place of `MK3D_` and `XBE_`; the game's side of it is
`sc5/src/sc5/link.cpp` and `autopilot.cpp` (docs/GAME.md, "The launcher's game": `mode=versus`, `sparring` and
`practice`, `watch=1` with the spectator feed, the plain games of `SC5_TRAINING` and `SC5_VERSUS`, the display and
sound variables, `controls.toml`; `tools/linktest.py` runs two games, and a spectator's, through it), and
`launcher/tools/fake_game.py` speaks it without the game.

### 3.1 Environment

| variable | value |
|---|---|
| `SC5_NET` | `1`: launcher-controlled; boot to the hold point and follow `cmd.txt`. Absent: a plain game (single player, practice) |
| `SC5_GAME` | `sc5` |
| `SC5_SESSION_DIR` | absolute path of the per-session folder |
| `SC5_CMD` | `<session>\cmd.txt` (with `SC5_NET=1`) |
| `SC5_STATUS` | `<session>\status.txt` (with `SC5_NET=1`) |
| `SC5_INSTANCE` | `1` normally; `1` and `2` for the two instances of a local test |
| `SC5_LOG` | `<session>\game.log`: the game writes its log there |
| `SC5_GAME_DIR` | the extracted disc, `<game folder>\disc` (3.7): `default.xex` with the title update's `default.xexp` beside it, which the runtime applies as it loads the executable; mounted as `game:` |
| `SC5_UPDATE_DIR` | `<game folder>\update`: the title update's other files (`patch.cpk`), mounted as `update:` |
| `SC5_DLC_DIR` | `<game folder>\dlc`: the player's own downloadable content, one folder per package with its files and the package's header beside it (3.7); empty or missing: none |
| `SC5_USER_DIR` | the saves: `<game folder>\user` for a plain game; with `SC5_NET=1` a fresh `<session>\user`, so both players' games start from the same (empty) save and nothing online changes the player's own |
| `SC5_CREATION_SAVE` | with `SC5_NET=1`: the player's own save, `<game folder>\user\<profile>\4E4D083D\00000001\SC5SaveData\savedata.bin` (the newest of the profiles'), where Creation keeps their created characters. The game only reads it (shared, never locked or written) and brings their characters into its fresh save in memory (3.3, *Created characters*); missing: none |
| `SC5_CACHE_DIR` | `<game folder>\cache`: the shader cache, kept between sessions (the game's default is `<SC5_USER_DIR>\cache`, which online would start empty every time) |
| `SC5_CAPTURE_DIR` | `<session>\captures`: F3 in the game saves what it drew there (1.13.2) |
| `SC5_DISPLAY` | `windowed`, `borderless` or `fullscreen` |
| `SC5_RESOLUTION` | `desktop` (the monitor's own) or e.g. `1920x1080`: the window's size, or the full screen's mode; a window at `desktop`, or one too large for the screen, is as large as fits on it with its frame (the same shape) |
| `SC5_RENDER_SCALE` | `1` to `4`: the internal resolution, times the console's 1280x720 (`2` is 2560x1440); the runtime's `resolution_scale` (`draw_resolution_scale_x`/`_y`) |
| `SC5_ANTI_ALIASING` | `off`, `fxaa` or `fxaa_extreme`: smoothing of the finished frame (the presenter's effect) |
| `SC5_ANISOTROPIC` | `0` (the game's own filtering) or `2`, `4`, `8`, `16` times anisotropic filtering (the runtime's `anisotropic_override`) |
| `SC5_VSYNC` | `0` or `1` |
| `SC5_ADAPTIVE_SYNC` | `0` or `1`: with v-sync, a G-SYNC or FreeSync monitor: the swap chain allows tearing, which such a monitor needs to follow the game; `0`: nothing makes a frame tear while v-sync is on |
| `SC5_BRIGHTNESS` | `50` to `200`, percent (100: as the game draws it): a gamma curve over the game's own ramp, blacks and whites kept |
| `SC5_FRAMEDATA` | `0` or `1`: the frame meter in practice (Training and Online Practice): both players' frames as Street Fighter 6's (startup, impact, recovery, stun), each player's startup, impact, recovery, total and advantage, and the damage panel; F7 in the game shows or hides it |
| `SC5_VOLUME_MASTER`, `SC5_VOLUME_MUSIC`, `SC5_VOLUME_EFFECTS`, `SC5_VOLUME_VOICES` | `0` to `150`, percent of the game's own level (voices include the announcer) |
| `SC5_DISPLAY_FILE` | the launcher's `display.txt` (beside its settings file): the display and sound settings again, as `key=value` lines (`mode`, `resolution`, `render_scale`, `anti_aliasing`, `anisotropic`, `vsync`, `adaptive_sync`, `brightness`, `frame_data`, `volume_master`, `volume_music`, `volume_effects`, `volume_voices`), replaced whole (3.2) at every change; see below |
| `SC5_CONTROLS` | the launcher's `controls.toml`: what keys and controller buttons press (below) |
| `SC5_TRAINING` | `1`, a plain game for practice (the PRACTICE button, or practice while a search waits): the autopilot goes to training's character select with every character unlocked and hands it to the player (port 0 picks both; only player 1 takes part) |
| `SC5_VERSUS` | `1`, a plain game for the offline versus (the VERSUS button): the two-player select screen with every character and stage unlocked, player 2 on the controller `controls.toml` names |
| `SC5_NET_LOG` | with `SC5_NET=1`, unless already set: `<session>\net-{match}.log`, every online match's frames (both players' inputs, the state checks; `{match}` becomes the match id) |

The working directory is the folder of the game executable. The launcher creates one session folder per game
process (`<launcher dir>\sessions\sc5-<yyyyMMdd-hhmmss>-<instance>`, the newest 30 kept). A `.py` executable (the
stand-in) is run with the configured Python and no window.

**What the display settings may change.** Two players' games compare their fight state every 30 frames, so a
setting the launcher passes may change only how a frame looks, never what the game computes. Every variable above
lives in the GPU emulation and the presentation, which the game does not read back: the internal resolution, the
anti-aliasing, the texture filtering, v-sync, the window and the brightness; the sound levels live in CRI's mix and
the audio the game hands the system (the game's own records of its Sound options stay untouched). Two players may set
them differently in one match. Nothing the game itself decides (its effect, shadow or particle settings) is offered; GPU answers the
game logic reads (occlusion queries: MK9 Online's desyncs) are fixed, the same on every PC, whatever the settings.

The display variables override the game's own configuration; a game started by hand keeps that alone (and the
command line overrides them). A game started with `SC5_DISPLAY_FILE` reads that file again whenever it changes (every
0.5 s it looks), so a game that stays up between matches follows the Settings page: v-sync, brightness, the texture
filtering and the four volumes at once, the window's mode and size the next time the game's window is the
foreground window (a change made in the launcher never pulls the game over it or switches the display's mode behind
it), the internal resolution, anti-aliasing and adaptive sync at its next start.

What SCV's game does with them (sc5/src/sc5/display.cpp): `borderless` and `fullscreen` are both a borderless
window over the whole screen (the SDK has no exclusive mode); the internal resolution, anti-aliasing (the swap's
FXAA), texture filtering and v-sync are the GPU plugin's own options (`resolution_scale`, `swap_post_effect`,
`anisotropic_override`, `vsync`), given at the start as their `REX_` environment variables; brightness is a curve
over the game's own gamma ramp (both of the GPU's ramp forms), blacks and whites kept; the master volume scales the
frames the game's audio hands the system. The music, effects and voices levels scale the volumes the game gives its
four main sound categories in CRI (sc5/src/sc5/sound.cpp): music is BGM (and the movies, which play at the game's
music level), effects SE and SYSTEMSE (the menus' sounds), voices VOICE (the fighters and the announcer). They
multiply the game's own Sound options, which stay as the player set them. `SC5_FRAMEDATA` (and `frame_data`, at once)
shows or hides the frame meter over Training and Online Practice (sc5/src/sc5/framemeter.cpp, docs/GAME.md "Frame data
in practice"): it only reads the fighters' memory, so two players may set it differently too.

**`controls.toml`** (the Controls page writes it; a running game reads it again within a second of a change):

```toml
keyboard = true                  # the keyboard plays as player 1 while the game's window is in front
[keys]                           # Windows virtual key names, at most two per button
up = ["Up", "W"]                 # up down left right (the d-pad), a b x y, lb rb (bumpers), lt rt (triggers),
a = ["K", "Space"]               # start back, ls rs (stick clicks): the Xbox 360 controller's buttons
[buttons]                        # what each button of a PC controller presses, by where it sits ("" nothing):
south = "a"                      # south east west north, left_shoulder right_shoulder, left_trigger right_trigger,
right_shoulder = "rb"            # dpad_up dpad_down dpad_left dpad_right, start back, left_stick right_stick,
paddle1 = ""                     # misc, paddle1-4, touchpad
[ports]
player2 = ""                     # player 2's controller (offline versus) by its name; "" the second plugged in
[fight]                          # what a button presses in a fight: the game's own four together ("" nothing)
lb = "A+B+K+G"                   # a b x y lb rb lt rt ls rs; A horizontal, B vertical, K kick, G guard
```

`[fight]` is the player's own button settings for fights, in every mode, in place of the game's Control Settings (an
online game's fresh save keeps those at the console's Type A, and they offer only 13 of the 16 combinations of A, B, K
and G; the stick clicks none). A button it does not list keeps what the game's own Control Settings give it: Type A
online (A guard, B kick, X horizontal attack, Y vertical attack, LB A+G, LT B+G, RB A+B, RT A+B+K, the stick clicks
nothing), offline what the player set in the game. Online the game turns the player's own pad into Type A's face
buttons before its netplay sends it (each listed button presses the face buttons that make its combination: X for A,
Y for B, B for K, A for G, and nothing of its own), so the other game and the spectators see exactly the combination
the player meant whatever each of them set; offline the battle's own conversion of player 1's pad takes it
(sc5/src/sc5/controls.cpp, docs/GAME.md "Controls in a fight"). Menus never see it.

With `SC5_NET=1` the game reports a controller on port 2 whether one is plugged in or not: the autopilot picks
player 2 there, and online the other player's inputs play it. SCV's game (sc5/src/sc5/controls.cpp) reads the
keyboard with the game's window in front, remaps every controller's buttons (paddles, `misc` and `touchpad` through
SDL, by the controller's name), and assigns the controllers to players in place of the SDK: the named player 2
controller as player 2, every other one in the order plugged in (player 1 first, then 3 and 4).

### 3.2 File rules (both files)

- ASCII, `\n` terminated (`\r\n` tolerated), no BOM. `cmd.txt` is one line. `status.txt` may carry further
  `key=value` lines after the first one (informational, for example `ping=38` or `rollback=2`); the launcher
  interprets only the first line.
- **Atomic replacement**: write `<file>.tmp` in the same folder, close it, then
  `MoveFileExW(tmp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`. If the replace fails because a reader
  holds the file open, retry every 5 ms for up to 1 s. Readers open with
  `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`.
- Line grammar: a verb (or state) followed by space-separated `key=value` tokens. Values contain no spaces
  (`[A-Za-z0-9_.:-]`); unknown keys are ignored; keys may come in any order.

### 3.3 `cmd.txt` (launcher -> game)

The game polls for `cmd.txt` at least every 100 ms. It **claims** a command by renaming `cmd.txt` to
`cmd.txt.taken` (replacing an older one), then reads and executes it. Claim-by-rename means a command written while
the game reads can never be lost. The launcher writes a new command only after the previous one was taken, or after
2 s; a newer untaken command replaces the older one (latest wins).

| command | meaning |
|---|---|
| `start game=sc5 mode=versus side=1 local_port=7001 remote=1.2.3.4:7002 delay=2 seed=123456 p1=Siegfried p2=- rounds=3 ranked=1 match=<id>` | start an online match from the hold point |
| `start game=sc5 mode=versus watch=1 feed=127.0.0.1:50210 seed=123456 p1=Siegfried p2=- rounds=3 ranked=0 match=<id>` | watch a lobby match (a spectator, 1.10.1) |
| `start game=sc5 mode=practice side=1 p1=Siegfried p2=Mitsurugi` | start offline practice (no network keys; the local test's practice) |
| `park` | leave the current match or practice and return to the hold point; after a result while the match's end (the winner's pose) still plays in sync, once both games have left the match together (30 s at most); the players' controllers keep working until then |
| `rematch [seed=N] [match=<id>] [p1=.. p2=..] [stage=keep]` | play again against the same peer from the result screen: per side the same fighter again (a created character by its checksum, picked again by its player's own game), or the one `p1`/`p2` names (a roster id, the game's id with its costume, `12.1`, or a created character, `c.<checksum>`), or `-`: that side's player picks again on the select screen; `stage=keep`: the last match's stage (the launcher's REMATCH). The same two fighters on the same stage are VS Battle's own Rematch, as on the console (no loading: about 16 s from the end of the match to the fight); anything else goes through VS Battle's Character Select and the stage select, where the last match's loser picks the stage (3.3, *The stage*); new seed and id when given. The launcher sends it for a series' next match when both games still wait at their result screens (1.9.1, `in_place`), and for a lobby's next match its loser chose there (1.10, `choose=1` again) |
| `quit` | exit the process cleanly |

`start` keys: `game` (`sc5`); `mode` (`versus`, `sparring`, `practice`); `side` (local player, 1 or 2); `local_port`
(UDP port to bind); `remote` (peer `ip:port`); `delay` (input delay in frames: `match_go`'s, the same in both games,
the larger of both players' settings, an Auto one's from the match's ping, 1.8; each game tells the other its delay
when they meet, and both play the larger of the two); `seed` (32 bit, identical on both
sides); `p1`/`p2` (roster ids from `launcher/data/sc5.json`, identical on both sides, or `-`: the player of that side
picks on the game's own select screen, below; an id with a costume after a dot, `12.1`, is the game's own character id
from a `result` line, which the autopilot picks in both games with that costume: a ranked set's winner keeping theirs;
`c.<checksum>`, a created character as a `result` line names it: the player's own game picks it for them again and the
other game follows that pick as any of the player's, the record coming again; the game's ids are `game_id` in
`launcher/data/sc5.json` and the table in docs/GAME.md; Devil Jin is not on SCV's select screen, a style for created
characters only, so no roster id names him); `rounds` (best of 1, 3 or 5: the game plays first to (N+1)/2 rounds);
`ranked` (0 or 1); `match` (id, echoed in status lines). Optional: `p1_name`, `p2_name` (account names; the game may
show them over the health bars), `p1_rank`, `p2_rank` (each player's ranked title in lower case, dashes for spaces,
`soul-warrior`, from `match_go`'s or `spectate`'s `p1_title` and `p2_title`; `-` none: the badges are
`ranks/<rank>.png` beside the launcher in a player package), `stage` (ignored: the players pick the stage on the
game's stage select, below), `feed` (`127.0.0.1:port`, a TCP port of the launcher's: the game writes its spectator
feed there, 3.6; a lobby match's), `watch=1` (a spectator: no `side`, `local_port` or `remote`; the game reads both
players' feed from `feed`), `choose=1` (a lobby's match the launcher keeps at its result screen, 1.10: the match's
loser chooses the next one there, below). Both sides get the same line except for `side`, `local_port`, `remote` and
`feed`. (MK3D's start line had `traps`, the death traps; SCV's has none.)

**Picks on the game's select screen** (`p1` and/or `p2` = `-`, the launcher's *Pick my character on the game's own
select screen*, its default, sent to the server as an empty `character`). The autopilot takes both games to the
select screen. There the player of a `-` side steers their own cursor with their own controller (port 0 and the
keyboard, routed to port 2 when they are P2; BACK is held back, and B but on the costume panel, as they would leave
the select screen and the match; B on the costume panel goes back to the characters, as on the console, and player
1's B after their whole pick, while player 2 still picks, takes it back in both games: the other game agrees over the
netplay unless player 2 finished first); the game passes their pick (the character and whatever else both games must have alike: the costume,
the stage the player chose) to the other game over its netplay, and the other game's autopilot makes that pick for
that side, its cursor going the same way. A side with a roster id is picked by the autopilot on both PCs, so the two
ways mix. Nobody's time runs out at the select screen online. Every character and stage is open in an online match
without changing any save, and the paid downloadable content only when both players have it (`SC5_DLC_DIR`): a pick
the other game cannot load is refused at the select screen. Both games compare the stage and the costumes when the
fight starts.

**Created characters.** At the hold point an online game brings the player's own created characters from their offline
save (`SC5_CREATION_SAVE`) into its fresh save in memory, slots 1 to 48 as there (49 and 50 stay free), each one whose
form is wrong (size, version, checksum, names) left out. At the select screen it reads what its own game can load from
the game's tables (the parts sets of each base type with the models they use, each fighter's own costumes among them,
the weapons of each style, the stickers with their textures, the palette's size, the characters on its select screen;
only what the game says is released on this PC: downloadable parts only with their package) and leaves out, before
anyone can pick it, any of the player's own that would not pass the checks below. The player picks one as on the
console: an original character on the Creation slot's costume panel, a customized fighter (a regular fighter
re-dressed in Creation) on its fighter's, after the fighter's own costumes. The game then sends the record (13,340
bytes: netplay packets 12 and 13, and the spectator feed's type `11`) and its PICK says so (costume `-2`). The other
game takes exactly the record's bytes from its 12 chunks (`sc5/src/sc5/net/created_transfer.h`, anything else refuses
it), then checks every field before anything loads it (`sc5/src/sc5/creation_check.h`): the form; the character type
and style on this PC; the limits the game's own repair pass keeps to (attack rate, voice, the body's sizes); the
body's rates 0-1; every parts set and every model among this PC's for it (or one of their files with segment 0xFF, a
piece Creation keeps worn but does not show; at most 20 models in break level 0, the fight's room for them) (an original character: its base type's, male
or female; a customized fighter: base type 5, one of that fighter's own two costumes in every break level, and the
hair any fighter may wear); a customized fighter's voice one the game knows; parts coordinates valid or not, with
finite values (the game checks their parts and bones itself); every sticker one of this PC's, with its own texture and
finite placement; every colour (the character's, its stickers', the weapon's, its trail's and effect's) none or on one
of the game's palettes; the weapon's other numbers finite; the weapon one of its style's. On top of that the game's
own repair pass (what a console runs on a save's records) runs on a copy, and a record it refuses or would change in
any byte is refused too. A player's own character left out at the select screen is said in the game, over the select
screen (large text: which one and why, in plain words), and the `starting` status line counts them (`held_out=N`,
3.4). Then it places the record in slot 49 (player 1's) or 50 (player 2's), and its autopilot picks
it for that side. As on the console, the picture Creation took is not sent: the select screen draws the other player's
card from the record itself, and on the VS screen, which shows the picture, that player's card shows SCV Online's own
(a figure with a question mark, in navy and gold). A record refused, one of another character than the pick, or one
that does not come within 20 s ends the match there: `error player N's created character from the other PC was
refused: <why>`. A created character with downloadable parts can be picked only when the other game's downloadable
parts are the same as this one's (a fingerprint in the PICK packet); else A on it is held back, as for Dampierre.
Picked, it fights wearing them in both games (GAME.md, "Downloadable parts in a fight"). Every match sends the records
again; the result line names a created character `c.<checksum>` (the same in both games), which a rematch and a ranked
set's winner pick again by that checksum. Tests: `SC5_TEST_CREATION_SLOT=<slot file>[,...]` puts whole slots (29,832
bytes) into the first slots at the hold point, `SC5_TEST_CREATION_RECORD=<record file>` places a record as the other
PC's would be for player 2, and `SC5_TEST_CREATION_UNCHECKED=1` takes any record of the right size (for fuzzing the
game's own handling, never in a real match; `=check` skips only the checks above, the game's repair pass still runs);
`sc5\out\build\win-amd64-release\creation_check_test.exe sc5\tests\data` tests the check and the transfer,
`tools\creationfuzz.py` what two games do with records garbled one area at a time (`--record patroklos`: the
customized Patroklos, picked on his costume panel, and the fighter cases); `SC5_TEST_KNOWN_OUT=<file>` writes what the
game read for the check, as text (`sc5\tests\data\known.txt`, the test PC's, which the test checks the game's records
against).

**The stage.** After the select screen, one player picks the stage on the game's stage select with their
own controller (LEFT, RIGHT and A) while the other game follows: player 1 in a match from a `start` line, then the
last match's loser in a `rematch` that goes through Character Select (player 1 after a draw). Random on it is drawn
from the match's seed, the same in both games, and so is the stage of a player who has not picked after a minute.
VS Battle's own Rematch, and `stage=keep`, keep the last match's stage.

**The way into a fight** (0.1.4): the VS screen while the fight loads, the stage's fly-over and the fighters' intros
play, as on the console, unless a player presses START on their own controller: both games skip them then (and the
spectators', from the feed). A KO's replay plays too, and a player's START or A cuts it short in both games and the
spectators' on a frame the two games agree on (docs/GAME.md, "Netplay").

**The loser's choice** (`choose=1`, a lobby's match, 1.10): after the match the game stays at its result screen. Once
the Results screen is up, the match's loser (player 1 after a draw) chooses REMATCH or CHARACTER SELECT with their own
controller (UP, DOWN, then A) on a panel the game draws over the Results screen, which both players see with the
loser's cursor on it; the other game follows the choice over the netplay. Both games then write `chose
choice=rematch|select match=<id>` (3.4) and wait for the launcher's `rematch`, whose way through the game's own menu
(Rematch, or Character Select) the two games take on the same frames. No choice: the launcher's `park` after a
minute.

### 3.4 `status.txt` (game -> launcher)

The launcher polls `status.txt` every 100 ms and acts on every change of the first line.

| first line | meaning |
|---|---|
| `booting` | process started, loading |
| `ready` | parked at the hold point, able to take `start` |
| `starting match=<id>` | took a `start`: off the hold point, setting the match up (picks, loading), until `in_match`; `held_out=N` after it (and after `in_match`'s) when N of the player's own created characters cannot be picked in this online game (3.3, *Created characters*; the game's log says which and why) |
| `in_match match=<id>` | a match (or practice: `match=practice`) is running; `again=N again_frame=F` after it once the two games went out of step and played the round again N times this match (SCV 0.1.4; the check of frame F failed last): the launcher sends a `desync_report` with `again` for each (1.13.1) |
| `result winner=1 p1_rounds=2 p2_rounds=1 p1_char=12.1 p2_char=7.0 match=<id>` | the match ended normally (`winner` is 1 or 2, or 0 for a draw: a drawn round, the time running out even or a double KO, took both to the rounds to win; `p1_rounds`/`p2_rounds`, each side's round points as the game counts them, a won round and a drawn one, which gives both players the point; `p1_char`/`p2_char`, the characters as the game's own ids with the costume after a dot, `3.0` Siegfried in his first costume, which `start` takes as `p1`/`p2` and the launcher names from `game_id` in its `data/sc5.json`; a created character as `c.<its checksum>`, 8 hex digits, the same in both games); the game stays on its result screen until `park`, `rematch` or `quit` |
| `sparring_over match=<id>` | Online Practice: the match in sync is over, a player pressed BACK (the launcher reports the outcome `sparring`) |
| `chose choice=rematch match=<id>` | after a `result` with `choose=1` (3.3, *The loser's choice*): the match's loser chose the next match on the result screen, `rematch` or `select` (Character Select); both games say it, and wait at their result screens for `rematch` or `park` |
| `desync frame=N match=<id>` | the two simulations diverged at frame N and the match is aborted: from SCV 0.1.4 only when the round could not be played again (`in_match ... again=`, above: three times a match at most, not in Online Practice) |
| `disconnected match=<id>` | the peer stopped answering |
| `error <text>` | something failed (`error no connection`, `error bad command`); free text to the end of the line |

`match=<id>` (echoed from `start`) is recommended on the match states so that identical results of two consecutive
matches are still different lines. A spectator's game reports the same states for the match it watches: `starting`,
`in_match` once both players' READY came through the feed, `result` (or `sparring_over`, `desync`) at its end.

Typical sequences: `booting` -> `ready` -> (`start`) -> `starting` -> `in_match` -> `result ...` -> (`park`) ->
`ready`; practice: `ready` -> (`start mode=practice`) -> `in_match match=practice` -> (`park`, or the player leaves
practice in the game) -> `ready`.

Beside the status, an online match's game writes `stats-<match>.json` into its session folder (`SC5_SESSION_DIR`;
the id's characters other than letters, digits, `-` and `_` as `_`), one line of JSON replaced whole (3.2): every
10 s while the match runs in sync (`"end":"running"`) and at its end, before its status line (`"end":"result"`,
`"sparring_over"`, `"desync"`, `"disconnected"`): `v`, `match`, `role` (`player1`, `player2`, `spectator`),
`netcode`, `delay`, `secs`, `frames`, `rollbacks`, `rerun`, `max_rollback`, `rollback_ms_avg`,
`rollback_longest_ms`, `waits`, `held`. A game closed or crashed in a match leaves the last one written. The launcher
does not read it yet (MK9 Online's sends it with the match's report).

The launcher reports `result`, `desync` and `disconnected` to the server (1.9), shows every state in its footer and
log, sends `park` once the server has answered with `match_result` (or after 10 s), and treats an exit of the
process as `error exited` (reported as `left` during a match, with how the game ended, from the last lines of its
log).

How long the launcher waits (the game's own times, docs/GAME.md, 2026-11-17: boot to `ready` about 40 s, `start` to
`in_match` 31-38 s, `park` to `ready` about 20 s): a match's game still coming up when the match goes gets 2 minutes to
reach `ready`, on top of the start's own time; from the `start` line on, 2 minutes to `in_match` (5 minutes when a side
picks on the select screen: nobody's time runs out there); after a result, 45 s for `park` to bring it back to
`ready`. A start that runs out is reported as the outcome `error`.

### 3.5 Local test

For development the launcher starts two instances on one PC. Once both report `ready`, instance 1 gets `side=1
local_port=7001 remote=127.0.0.1:7002` and instance 2 gets `side=2 local_port=7002 remote=127.0.0.1:7001`, both
with the same `seed`, `p1`, `p2`, `rounds`, `delay` and `match=local-<time>`. No server is involved.

### 3.6 The spectator feed

A lobby match's `start` carries `feed=127.0.0.1:<port>`: a TCP port the launcher listens on (`FeedLink`). A
player's game connects to it (again every 0.5 s when the connection drops) and writes its feed, whole packets each
after its little-endian `u16` length; the launcher passes them to the server every 0.1 s (1.10.1). A spectator's
game connects to its own launcher's port and reads both players' packets, everything delivered so far again
whenever it connects.

Each packet starts with the header the server checks: the 4 bytes `SC5R`, the type (`u8`), the side that wrote it
(`u8`, the netplay's player index: `0` for the start line's `side=1`, `1` for `side=2`; a launcher may send only its
own side's), the barrier number (`u16` LE: which meeting of the two games, a match and each rematch counting up);
then the type's payload, which only the games read:

| type | payload |
|---|---|
| `5` | the player's pick on the select screen: `u8 side, i8 phase, i8 slot, i8 costume, u32 pickable` (the coming match's number), as it changes |
| `6` | the meeting, by a first `u8` kind: `0` READY (`u32 seed, u8 n, n x u32 words, u8 delay`, each game at the barrier); `1` SETUP (`u32 stage, u32 round time, u32 wins, u8 n, n x u32`, player 1's battle setup); `2` BLOCK (`u16 chunk, u16 chunks, u32 source, u16 offset, u16 len, len bytes`: player 1's whole fight at the barrier) |
| `7` | the player's own inputs: `u32 first frame, u8 n, n x (u32 buttons | lt << 16 | rt << 24, u32 sticks lx ly rx ry)` |
| `8` | the game's state checks: `u32 frame, u32 hash low, u32 hash high` |
| `9` | the frame the game left the match at: `u32 frame` |
| `10` | the stage select, by the player whose turn it is: `u8 side, i8 phase, i8 cursor, i8 stage` (the coming match's number; phase `0` the cursor as it moves, `1` the place taken, 0 Random and 1-27 the stages to its right), as it changes |
| `11` | the player's created character for the coming match (their pick's costume `-2`): `u8 side, u8 chunk, u8 chunks, u16 len, len bytes`, each of its 12 chunks once (3.3, *Created characters*); a spectator's game checks and places it as a player's does |
| `12` | a skip (0.1.4): `u8 kind`; kind `0`, the player skipped the way into the coming match (the VS screen, fly-over and intros); kind `1`, `u32 frame`: a KO's replay of this match cut short at that frame, as both players' games agreed (a spectator's game presses there too) |

A lobby's match that went on from the last one's result screens (1.10) is its two games' second meeting or a later
one, and its packets carry that number; a spectator's game, started for that match, counts from the number its feed
brings. (The server took only types 5 to 9 until 0.1.4: the stage select's and created characters' packets, and the
batches they came in, were refused, and spectators never got the stage a player picked.)

A spectator's game takes both picks and the stage select's from the feed, takes player 1's battle setup at its own
(the stage, round time, rounds and generators, as player 2's game does over the netplay), meets both games' READY
words where a player's game meets the other's, takes the counters (each the larger of the two games'), player 1's
battle state machine and whole fight there (player 2's game takes only the blocks that differ in its own; a
spectator cannot ask), and plays every frame from both players' inputs, never predicting: a frame waits for both
inputs, and the frames the players ran on neutral inputs (outside a round's fighting) it runs so too. A game far
behind (a spectator who came in late) runs frames unseen until it is within half a second of the players. Its own
state checks are compared with the players'. A match's feed is about 250 KB for a one-round fight (8,300 packets;
player 1's fight about 90 KB of it).

### 3.7 What Build game lays out

The launcher's Build game page (`launcher/src/ToolPages.cpp` `BuildPage`, `X360.cpp`) takes the player's own files
and checks them before it writes anything; it refuses anything else (another region, print or update), since both
players of a match must run the same code:

- the disc image (`.iso`: an XGD2 or XGD3 dump or an XISO; the GDFX file system): its `default.xex` must have SHA-1
  `dfe4d5e1e9198bc9df6db46ddf2bdf2a5cbf3f80` (title `4E4D083D`, media `165FAD4F`, version 0.0.0.2);
- the title update package: STFS `LIVE`, content type `000B0000`, title `4E4D083D`; its `default.xexp` must have
  SHA-1 `47a09662fd3fe376d283208fcd8a31eee819c736` and a delta patch from 0.0.0.2 to 0.0.1.2, its `patch.cpk`
  SHA-1 `0b2b9d30ce56c02fafa91a0f4fbdaeb23d441a93`;
- optionally the player's own downloadable content packages: STFS (`LIVE`, `PIRS` or `CON`), content type
  `00000002`, title `4E4D083D`.

In the game folder (`games\sc5` beside the launcher in a player package, the repository's `game\` in a source
build) it writes:

| path | what |
|---|---|
| `disc\` | the disc's files, the dashboard update (`$SystemUpdate`) left out; a file already there with the right size is kept (a stopped extraction goes on) |
| `disc\default.xexp` | the title update's patch, beside `default.xex` |
| `update\patch.cpk` | the title update's other file |
| `dlc\<package>\` | each DLC package's files, as its file table has them (`<package>`: the package's file name): its index, and a costume pack's data files (those `tools/py/ps3dlc.py` converts from a player's PS3 packs: up to 120 MB a package). The game links them into its saves at startup (GAME.md) |
| `dlc\<package>.header` | the package's header (its first `header_size` bytes: the content type, title, names and license the console lists it by) |

Then one more step compiles the game's code for this PC (`BuildPage::compileGame`): it runs the game folder's
`build-game.cmd` (Settings, *Build script*) with `SC5_GAME_DIR`, `SC5_UPDATE_DIR` and `SC5_GAME_EXE` (where the
built `sc5.exe` goes) in its environment, and takes exit code 0 with `sc5.exe` there as built. A player package has
that script (`tools/player/build-game.cmd`; sc5/player/README.md): with nothing installed on the PC it translates
`SC5_GAME_DIR`'s `default.xex`, patched by the `default.xexp` beside it (no `default.xexp`: it stops, exit code 1),
compiles that code into `sc5game.dll` with the package's own clang, and puts the package's ready-made `sc5.exe`,
`sc5game.dll` and the runtime's DLLs in `SC5_GAME_EXE`'s folder; `SC5_GAME_EXE` must name `sc5.exe`. It prints one
line per step for the page's log, and `BUILD OK` at the end; the first build takes a minute or a few, a build with
nothing changed seconds. In a package `SC5_GAME_EXE` is `games\sc5\sc5.exe` also before the first build. A source
build has no `build-game.cmd` in its game folder (`sc5\build.cmd` builds the game there), so the page stops after the
files, saying the compile step is not ready.
