# TLOU2 Campaign Co-op (work in progress)

A two-player campaign co-op mod for **The Last of Us Part II Remastered** (PC), built on the client
source of [Twelve Weeks](https://github.com/kelliex3/Twelve-Weeks) by Kellie and contributors
(GPL-3.0). The networking, remote-player puppets and animation replication are theirs; this fork
strips the multiplayer mode out and points that machinery at the single-player campaign.

Unofficial fan project, not affiliated with Sony Interactive Entertainment or Naughty Dog. You need
your own copy of the game on Steam.

**Test builds only.** Expect bugs. Back up your saves before playing.

## Download and install

1. Download the zip from the newest entry on the [Releases](../../releases/latest) page. Both
   players need the same build.
2. Close the game. Open the game folder: Steam > Library > right-click The Last of Us Part II >
   Manage > Browse local files.
3. If a `winmm.dll` is already there (for example from Twelve Weeks), rename it to `winmm.dll.bak`.
4. Copy `winmm.dll` and `tlou2-coop.ini` from the zip into that folder.
5. Start the game from Steam (not from the Twelve Weeks launcher, which puts its own `winmm.dll` back).

To uninstall, delete `winmm.dll` and `tlou2-coop.ini` (and rename `winmm.dll.bak` back if you had one).

## How to play

- Both players are **Steam friends**.
- **Host:** load into the campaign, then press **F7**.
- **Friend:** press **F8**, even on the main menu. The game loads the host's chapter and puts you
  next to the host.
- **F6** leaves. The status line in the top-left corner shows what is happening.

No port forwarding, IP addresses or extra programs: it uses Steam's own peer-to-peer networking
(direct connection or a Valve relay) through the game's own Steam library. For LAN play without
Steam, set `join_ip` in `tlou2-coop.ini` to the host's IP; F8 then joins that IP on UDP 7777.

Everything the mod does is logged to `tlou2-coop.log` next to `tlou-ii.exe`. Send that file
along with any bug report.

### tlou2-coop.ini

```ini
name=Player     ; shown to your partner; Player = your Steam name
look=auto       ; how your partner sees you; auto = your current campaign character
join_ip=        ; empty = F8 joins a Steam friend; an IP = LAN join on UDP 7777
spawner=        ; optional: NPC spawner names to borrow for the partner character
overlay=1       ; status line in the top-left corner
nametags=1      ; your partner's name above their character
friendlyfire=0  ; 1 = your bullets hurt your partner (the host's setting counts for both)
```

## What works

- **See each other:** your partner is a character in your world that copies their movement,
  animation, held weapon and gunshots (muzzle flash, sound, arrows), with a name tag. This
  includes Seattle's open-world area.
- **Shared enemies:** the host's game runs the enemies. On the joined player's screen the same
  enemies follow the host's, walking, running, turning and firing with their own animations.
  Enemies see and attack both players. Damage and kills from either player apply to both.
  An enemy your partner hits with melee (or an explosion / thrown item) plays its hit reaction
  on your screen too.
- **Doors and pickups:** a door one player opens opens for both. An item one player picks up
  disappears for the other. If the host dies or reloads, items picked up since the checkpoint
  come back for both.
- **Joining late:** a player who joins mid-chapter gets the host's enemies (killed ones removed,
  new ones added), opened doors and taken items.
- **Chapters:** when the host moves to a new part of the story, the joined player follows. A joined
  player who gets to the next chapter first waits there up to 25 seconds for the host, then is taken
  back to the host if the host doesn't come.
- **Deaths and checkpoints:** if the host dies or reloads a checkpoint, the joined player reloads
  with them. The joined player doesn't die from damage: a killing hit puts them back next to the
  host instead, so both games stay in step.
- **Difficulty:** the joined player plays on the host's difficulty; their own setting comes back
  when they leave.
- **Saves:** while joined to someone else's game, your game doesn't save, so your own progress is safe.

## Known issues

- Both players can grab the same item if they take it at the same moment.
- Enemy melee, grabs, flinches and infected/dog special moves are not copied yet.
- Thrown items (bottles, bricks, molotovs) are not shown yet.
- Turning friendly fire off (the default) only stops bullets. Your arrows, explosions and melee
  can still hurt your partner.
- Cutscenes are not shared yet.
- Two players only.

## Roadmap

| Milestone | What | State |
|---|---|---|
| M1 | See each other: same chapter, partner character with movement, animation, weapon, name tag | done |
| M2 | Shared combat: host-run enemies, shared damage and kills, enemy animations and gunfire, death / reload sync, difficulty sync | done (testing) |
| M3 | Shared progress: doors, pickups, chapter changes, cutscenes and story moments | in progress |
| M4 | Polish: downed / revive, in-game join menu, thrown items, enemy melee and grabs, infected / dog specials | planned |
| M5 | **No Return** co-op: play roguelike runs together (same encounters, shared waves, run rewards) | planned |

Test keys still in the builds (will be removed): **F10** spawns a test copy of yourself
(Shift+F10: save-block test), **Shift+F6** an animation mirror test (the 2nd nearest enemy copies the nearest one's animation), **F2** makes enemies ignore you (blind and deaf; F2 again undoes it). `boot_task=<node>/<task>` in tlou2-coop.ini loads that story part straight from the main menu, with saving off, **F9** writes a spawner dump to the log (Shift+F9: chapter reload
test), **F11** an enemy movement test.

## Building

Needs CMake 3.25+, Ninja, LLVM (clang-cl, lld-link, llvm-ml64) and an installed MSVC + Windows SDK.

```powershell
cmake --preset ninja-release
cmake --build --preset ninja-release
.\install.ps1 -Config TO1Release
```

`local-config.cmake` (gitignored, see `local-config.cmake.example`) must set `TO1_GAME_INSTALLS`;
the post-build copy goes there. `install.ps1` moves an existing `winmm.dll` (for example Twelve
Weeks') into `tlou2-coop-backup\` first; `uninstall.ps1` puts it back. Unlike Twelve Weeks, no
`psarc/`, `widgets/`, `gui2.exe` or `ndarc.exe` is needed: the co-op build embeds no game data.

## What changed from the Twelve Weeks client

- No embedded archive, mod-menu level, matchmaking server, relay, tamper reports or hardware id.
  Since M3.7 that code is out of the build entirely: the anti-debugging checks, hardware-id
  collection, tamper reporting, relay ping, server connection/config fetch, account token and
  the multiplayer menu (with its "open the website" shell command) are deleted, and the few
  interfaces the remaining code still calls are inert stubs. A failed startup check now exits
  cleanly instead of hanging the game unless a debugger is attached. (Windows Defender's
  machine-learning scan flagged earlier builds as `Trojan:Script/Wacatac.H!ml`, a false
  positive caused by these patterns.)
- Saves, profile and player stats load normally (Twelve Weeks swaps them for its MP mode).
- Steam peer-to-peer transport (`src/net/online/net-steam.cpp`) through the game's own
  `steam_api64.dll`, with a friends-only "who is hosting" lookup.
- `src/coop/`: host/join, config, status overlay and name tags (`coop.cpp`), borrowing an NPC
  spawner from the loaded campaign level for the partner character, "join where the host is"
  chapter / checkpoint / death sync, guest save block, difficulty sync; shared enemies
  (`enemysync.cpp`); doors and pickups (`worldsync.cpp`).
- `NetTick` runs only the session plus `net::replication::CoopTick` (puppet replication, no PvP rules).
- `net::hooks::InstallCoopHooks`: puppet guards, damage routing, gunshot / projectile hooks and
  the door hook.

## License

GPL-3.0, same as Twelve Weeks. See [LICENSE](LICENSE).
