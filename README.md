# TLOU2 Campaign Co-op (work in progress)

A campaign co-op mod for **The Last of Us Part II Remastered** (PC), built on the client source of
[Twelve Weeks](https://github.com/kelliex3/Twelve-Weeks) by Kellie and contributors (GPL-3.0). The
networking, remote-player puppets and animation replication are theirs; this fork strips the
multiplayer mode out and points that machinery at the single-player campaign.

Unofficial fan project, not affiliated with Sony Interactive Entertainment or Naughty Dog. You need
your own copy of the game.

## Status: milestone 1

| Milestone | What | State |
|---|---|---|
| M1 | Both players load the same chapter; each sees the other as an NPC puppet (position, animation, held weapon) | in progress |
| M2 | Shared combat: host-owned enemies mirrored to the client, client hits sent to the host | planned |
| M3 | Shared progress: chapter / checkpoint sync, partner pulled along through cutscenes, doors and pickups | planned |
| M4 | Downed / revive, join menu, partner camera during cutscenes | planned |

In M1 enemies, pickups and story progress are still local to each player.

## Playing (M1 test builds)

1. Close the game, then run `install.ps1` (moves an existing `winmm.dll`, e.g. Twelve Weeks', into
   `tlou2-coop-backup\` first). `uninstall.ps1` puts it back.
2. Start the game from Steam (not the Twelve Weeks launcher, which reinstalls its own `winmm.dll`).
3. Both players are Steam friends and load the **same chapter / checkpoint**.
4. The host presses **F7**.
5. The friend presses **F8**: the mod finds the Steam friend who is hosting and joins them. No IP
   addresses, no port forwarding and no extra programs: it uses Steam's own peer-to-peer networking
   (direct connection or a Valve relay), through the game's own Steam library.
6. **F6** leaves. **F9** writes a spawner / look discovery dump to the log. **F10** spawns a test copy
   of yourself.

LAN without Steam: set `join_ip` in `tlou2-coop.ini` to the host's IP; F8 then joins that IP on
UDP 7777 instead.

Everything the mod does is logged to `tlou2-coop.log` next to `tlou-ii.exe`.

### tlou2-coop.ini

```ini
name=Player          ; shown to your partner; Player = your Steam name
look=auto            ; how your partner sees you; auto = your current campaign character
join_ip=             ; empty = F8 joins a Steam friend; an IP = LAN join on UDP 7777
spawner=             ; optional: NPC spawner names to borrow for the partner puppet
overlay=1            ; status line in the top-left corner
```

## Building

Needs CMake 3.25+, Ninja, LLVM (clang-cl, lld-link, llvm-ml64) and an installed MSVC + Windows SDK.

```powershell
cmake --preset ninja-debug
cmake --build --preset ninja-debug
.\install.ps1
```

`local-config.cmake` (gitignored) must set `TO1_GAME_INSTALLS`; the post-build copy goes there.
Unlike Twelve Weeks, no `psarc/`, `widgets/`, `gui2.exe` or `ndarc.exe` is needed: the co-op build
embeds no game data.

## What changed from the Twelve Weeks client

- No embedded archive, mod-menu level, matchmaking, relay, tamper reports or hardware id.
- Saves and player stats load normally (Twelve Weeks blocks them for its MP mode).
- `src/coop/`: host/join hotkeys, config, status overlay, and borrowing an NPC spawner from the
  loaded campaign level for the partner puppet (the MP client spawner only exists in Twelve Weeks' maps).
- `NetTick` runs only the session plus `net::replication::CoopTick` (puppet replication, no PvP rules).
- `net::hooks::InstallCoopHooks`: the puppet / engine-validity guards only.

## License

GPL-3.0, same as Twelve Weeks. See [LICENSE](LICENSE).
