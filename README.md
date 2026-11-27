# NHL 94 Online

*NHL Hockey '94* (Sega Genesis, 1993) played online the way EASHL plays: **every
person controls one player on the ice for the whole game**: a center, a winger,
a defenseman or the goalie. Up to 12 people (6 a side), and any spot nobody
takes is played by the computer, so 1v1, 3v2 or a full 6v6 all work.

It runs natively on macOS, Windows and Linux. The game is a static
recompilation of the original cartridge: the original game logic, compiled to
native code, with rollback netplay on top. You need your own copy of the ROM;
none is included.

This is an unofficial fan project, not affiliated with or endorsed by EA, the
NHL or the NHLPA.

## What's different from the original

**Your position, your player.** In the cartridge, a human always controls
whoever is nearest the puck. Here you pick a position in the lobby (Home C,
Away G, ...) and you play that skater all game. You never switch to someone
else.
- Line changes are automatic. You follow your position onto the ice.
- When your position is off the ice (a penalty, a short-handed line), you get
  a substitute skater and return to your own position when it comes back.
- If your goalie is pulled, the goalie's player skates as the extra attacker.
- Penalty shots and shootouts: the fouled player's human shoots, the goalie's
  human saves.
- 6v6 (the cartridge) or a 4v4 format (G, C, LW, LD a side).

**Drop in, drop out.**
- A player who disconnects is replaced by the CPU; the match goes on for
  everyone else.
- You can join a match that is already running: you catch up on what you
  missed and take a free (or CPU) position at the next whistle.
- The lobby has a Ready button and a Swap button to trade positions with
  another player (they have to accept).

**Builds.** In the lobby, choose any player from the 1994 rosters as your
"build", with a search box: skaters and goalies separately. Several people can
pick the same player (six Gretzkys is fine), and "Team default" uses your
team's own first-line player. Your build's ratings and name are used in the
game: on the ice, in goal banners, on name tags.

**Passing and teamwork.**
- Passes go where you aim them, preferring human teammates.
- Without the puck, the pass button is "call for pass": a CPU teammate with
  the puck passes to you.
- Teammates calling for the puck show a ring and an icon.

**Your own screen.**
- Name tags above every human-controlled player, with off-screen arrows.
- A camera that follows your own player, turned so your team always attacks
  up the screen, like modern hockey games. A key toggles it to follow the
  puck instead.

## Playing

1. Get a build (see Building below).
2. Supply your ROM: the USA/EU *NHL Hockey '94* cartridge dump, 1,048,576
   bytes, CRC32 `9438F5DD`. The launcher asks for it the first time.
3. **Netplay → Online Netplay**, set your name, then host a lobby or join one.
   Hosts choose the positions in **Mods → Control mode** (presets: 1v1, Full
   6v6, Full 4v4, or custom).
4. Everyone in a match needs the same build of the game.

Online play needs a lobby server. The one in `server/` supports everything
above (12-player rooms, joining a running match). Run your own (below) and
set its address in **Netplay → Network Settings**, or bake it into your builds
(`local.cmake.example`).

## Building

Requirements: CMake 3.20+, Ninja, a C/C++ compiler (Clang or GCC), Python 3
(the build generates the player tables from your ROM), and SDL2. Copy your ROM
to `game/NHL94.bin` first: the build reads it to generate the code and is
never redistributed by it.

```sh
git clone <this repository> nhl94-online && cd nhl94-online
cp /path/to/your/NHL94.bin game/NHL94.bin
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGEN_ENABLE_TRACE=OFF -DGENESIS_REVERSE_DEBUG=OFF \
  -DGENESISRECOMP_NETPLAY=ON -DGENESISRECOMP_NET_ICE=ON
cmake --build build --target NHL94Recomp
./build/NHL94Recomp
```

Portable macOS (universal) and Windows (cross-compiled from macOS) builds and
their packaging: `docs/MAC_BUILD.md`, `docs/WINDOWS_BUILD.md`.

### The lobby server

`server/` is a Rust service (WebSocket lobby, match start, optional TURN
credentials).

```sh
cd server
cargo build --release
BIND_ADDR=0.0.0.0:8765 ./target/release/recomp-net-server
```

Players connect to `ws://<your host>:8765`. See `server/README.md`,
`server/docs/` and `server/.env.example` (TURN relay for players behind strict
NATs: `server/docs/COTURN.md`).

## How it works

- `game/`: everything NHL 94 specific, written as small C hooks on the
  original code (the generated C is never edited). Position lock:
  `nhl94_seats.c`; builds: `nhl94_builds.c`; views: `nhl94_view.c`; name tags:
  `nhl94_tags.c`; 4v4: `nhl94_format.c`.
- `segagenesisrecomp/`: the Genesis recompiler and runtime, with
  `external/recomp-net` (rollback netplay, lobby client, drop-in/late join)
  and the CPU cores.
- `recomp-ui/`: the launcher and lobby UI.
- `server/`: the lobby server.

Design notes: `docs/PLAYER_LOCK.md` (positions, substitutes, penalty shots),
`docs/BUILDS.md` (player builds), `docs/DROPIN.md` (disconnects, joining a
running match, Ready/Swap), `docs/VIEW_RESEARCH.md` (per-player camera).

Everything that changes the game is deterministic and part of the shared
simulation, so rollback netplay keeps every player in sync; cameras, name tags
and other display features are local to each player.

## Credits

Built on the work of many people:
- [NHL94Recomp](https://github.com/Mhopkinsinc/NHL94Recomp) by **McMarkis
  (Mhopkinsinc)**, and the
  [segagenesisrecomp](https://github.com/mstan/segagenesisrecomp) engine by
  **Matthew Stanley**.
- [recomp-net](https://github.com/RetroPortingToolKit/recomp-net),
  [recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui),
  [recomp-net-server](https://github.com/RetroPortingToolKit/recomp-net-server)
  and rbengine by **RetroPortingToolKit**.
- The reverse-engineered NHL 94 source,
  [NHL94Genesis](https://github.com/abdulahmad/NHL94Genesis), by
  **abdulahmad**, with the work of **chaos**, **StaticEck**, **McMarkis** and
  **Drezz**, and thanks to **smozoma** and **kingraph** and the NHL 94 modding
  community.
- The original developers of *NHL Hockey '94*.

## License

Mixed, per directory; see `NOTICE.md`. The game code, engine and CPU cores are
[PolyForm Noncommercial 1.0.0](https://polyformproject.org/licenses/noncommercial/1.0.0/):
free to use, change and share for any non-commercial purpose. recomp-net,
recomp-ui, rbengine and the lobby server are MIT. No ROM or other game data is
included.
