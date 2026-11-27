# Windows x64 build (cross-compiled from macOS) and the matching macOS build

The upstream Windows build is MSVC (`_build_release.bat`). This page is the
other route: a MinGW-w64 **cross-build on a Mac**, plus a macOS build of the
same tree, both with internet netplay (ICE), that can join the same match.

## What makes the two builds compatible

The lobby only lets identical builds meet: `game_version` must match exactly,
and the engine's default version is `"<release>+<FNV-1a of the exe bytes>"`
(`runner/netplay/genesis_netplay_identity.c` -> `runtime_evidence.c`), which a
Mach-O and a PE can never share.

`NHL94_NETPLAY_SOURCE_IDENTITY` (CMake option, default ON, netplay builds only)
replaces that fingerprint with a hash of what the binary is built **from**:

- `cmake/NetplaySourceIdentity.cmake` runs before every build of
  `NHL94Recomp` (target `NHL94Recomp_netplay_identity`) and hashes every
  `*.c/h/cpp/inc/toml/csv/txt/cmake` under `game/`, `segagenesisrecomp/runner`,
  `segagenesisrecomp/external/{m68k-recomp-core,recomp-net/src,recomp-net/include,rbengine}`,
  `segagenesisrecomp/recompiler/src`, `segagenesisrecomp/cmake`, `cmake/`,
  `recomp-ui/src`, plus `CMakeLists.txt`, `recomp-ui/recomp_ui.cmake`, the
  generated C (build-dir path in its header comment replaced by a token), and
  the options `GEN_ENABLE_TRACE`, `GENESIS_REVERSE_DEBUG`, ICE on/off.
  CR bytes are dropped so CRLF and LF checkouts agree.
- It writes `<build>/netplay_source_identity/genesis_netplay_source_identity.h`
  (`GENESIS_NETPLAY_SOURCE_FP` = first 32 bits of the SHA-256) and
  `...h.manifest.txt` (every file and its hash: diff two of these to see why
  two builds differ).
- The engine (`genesis_netplay_identity.c`, a 14-line opt-in change) uses
  that value for `genesis_identity_build_fp()` when
  `GENESIS_NETPLAY_SOURCE_IDENTITY` is defined; otherwise it keeps the exe hash.
  The same value is the rollback handshake's build fingerprint.

Any source edit, a different `game.toml`, or C regenerated from a different
ROM gives a different version, so genuinely different game code still never
meets. Determinism across compilers is not assumed: the ROM SHA-256 + session
config is the rollback content identity (refused on mismatch), and the boot
digest and per-tick timeline digests arbitrate in every match.

The runtime-evidence/diagnostic-replay identity is still the exe hash.

## Prerequisites (macOS, Apple Silicon or Intel)

```sh
brew install cmake ninja mingw-w64 sdl2   # sdl2 (or sdl2-compat) for the Mac build
mkdir -p ~/nhl94-online-build/deps/sdl2-mingw
cd ~/nhl94-online-build/deps/sdl2-mingw
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-devel-2.28.5-mingw.tar.gz
# sha256 3c0c655c2ebf67cad48fead72761d1601740ded30808952c3274ba223d226c21
tar xzf SDL2-devel-2.28.5-mingw.tar.gz
```

libjuice v1.7.2 is fetched by CMake (FetchContent, pinned hash) for both builds.
The ROM stays at `game/NHL94.bin`; nothing copies it.

## 1. macOS build (also provides the host recompiler)

```sh
B=~/nhl94-online-build
cmake -S ~/src/nhl94-online/NHL94Recomp -B $B/mac-ice -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DGEN_ENABLE_TRACE=OFF -DGENESIS_REVERSE_DEBUG=OFF \
  -DGENESISRECOMP_NETPLAY=ON -DGENESISRECOMP_NET_ICE=ON
cmake --build $B/mac-ice --target NHL94Recomp
```

`$B/mac-ice/GenesisRecomp` is the host 68K->C generator the Windows build uses.

## 2. Windows build (cross)

```sh
cmake -S ~/src/nhl94-online/NHL94Recomp -B $B/win64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/src/nhl94-online/NHL94Recomp/cmake/mingw-w64-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Release -DGEN_ENABLE_TRACE=OFF -DGENESIS_REVERSE_DEBUG=OFF \
  -DGENESISRECOMP_NETPLAY=ON -DGENESISRECOMP_NET_ICE=ON \
  -DNHL94_COPY_ROM_TO_BUILD=OFF \
  -DGENESIS_RECOMPILER_EXE=$B/mac-ice/GenesisRecomp \
  -DNHL94_SDL2_MINGW_PREFIX=$B/deps/sdl2-mingw/SDL2-2.28.5/x86_64-w64-mingw32
cmake --build $B/win64 --target NHL94Recomp
```

Both builds must use the same options (`GEN_ENABLE_TRACE`, `GENESIS_REVERSE_DEBUG`,
ICE) or their versions differ by design. Check:

```sh
grep FP $B/mac-ice/netplay_source_identity/*.h $B/win64/netplay_source_identity/*.h
diff $B/mac-ice/netplay_source_identity/*.manifest.txt $B/win64/netplay_source_identity/*.manifest.txt
x86_64-w64-mingw32-objdump -p $B/win64/NHL94Recomp.exe | grep "DLL Name"
```

The exe links libgcc, libstdc++ and winpthreads statically (`-static`); its only
non-system import is `SDL2.dll`. The UCRT (`api-ms-win-crt-*`) ships with
Windows 10/11. `-ffile-prefix-map` keeps this machine's paths out of the exe.

## 3. Package

```sh
SDL2_MINGW_ROOT=$B/deps/sdl2-mingw/SDL2-2.28.5 \
  tools/win64-online/package.sh $B/win64 $B/dist/NHL94Online-win64-<sha>.zip
```

Uses the engine's allowlist packager (`segagenesisrecomp/tools/package_release.py`,
refuses ROMs/saves/logs). Contents: `NHL94Recomp.exe`, `SDL2.dll`, `settings.ini`,
`NHL94-1v1-seats.bat`, `README.txt` (from `tools/win64-online/`), `LICENSE`,
`THIRD-PARTY-LICENSES.md`, `assets/`, `licenses/` (SDL2 zlib, libjuice MPL-2.0,
recomp-ui/ImGui/recomp-net/rbengine MIT, font/image notices, engine PolyForm +
PERMISSIONS, MinGW-w64 runtime, vendored runner deps).

## New CMake knobs (NHL94Recomp/CMakeLists.txt)

| option | default | meaning |
|---|---|---|
| `NHL94_LOBBY_URL` | env `NHL94_LOBBY_URL`, else `local.cmake`'s `NHL94_LOBBY_URL_DEFAULT`, else empty | netplay builds: compile `game/nhl94_netplay_defaults.c`, which sets `RNET_LOBBY_URL` at startup when neither `RNET_LOBBY_URL` nor `GENESIS_NET_LOBBY_URL` is set. The launcher's saved lobby URL (`saves/netplay/network settings`) still wins. |
| `NHL94_NETPLAY_SOURCE_IDENTITY` | ON | source-hash netplay identity (above) |
| `NHL94_COPY_ROM_TO_BUILD` | ON | Windows: copy the ROM next to the exe after a build (old behaviour; OFF for packaging builds) |
| `NHL94_SDL2_MINGW_PREFIX` | empty | MinGW: the `x86_64-w64-mingw32` dir of the SDL2 mingw package |

## On the Windows PC

1. Unzip anywhere (keep the folder together). Put your own NHL 94 ROM
   (1,048,576 bytes, CRC32 9438F5DD) anywhere, e.g. in the same folder.
2. Start `NHL94-1v1-seats.bat` (per-skater 1v1, `NHL94_SEATS=home:C,away:C`)
   or `NHL94Recomp.exe` (classic). The Mac side must use the same seat list:
   `NHL94_SEATS=home:C,away:C ./NHL94Recomp` (or no `NHL94_SEATS` for classic).
   NHL 94 has no `netplay_config_adopt`, so the guest does NOT take the host's
   seat list; a mismatch desyncs.
3. Pick the ROM in the launcher once; allow the exe through the firewall.
4. Launcher -> Netplay -> **Online Netplay** (no sign-in, see below).
   Host: **Host Lobby** -> name, Max Players 2 -> **Create Lobby**; wait for
   the guest. Guest: pick the room in the list -> **Join** (guests are
   readied automatically). Host presses **PLAY**. Two-seat online rooms run
   over ICE (the server puts every online room on the host relay over ICE).
5. Both consoles must print the same `genesis_netplay: lobby identity ...
   version=dev+<fp>` and, in the match, `peer identity matches`.

Fallback: Netplay -> LAN / Direct IP works across the internet only with the
host's UDP game port forwarded.

## Local patches: online without Discord sign-in (branches `nhl94-no-signin`)

Upstream recomp-ui `0c6a176` refuses every online lobby connect unless the
player is signed in with Discord. Our lobby server (recomp-net-server
`a39d1d4`, `REQUIRE_AUTH=false`, no `DISCORD_*`) offers no sign-in, so nobody
could go online. The capability signal used is the server's own:
`POST /auth/discord/start` answers **503** when Discord is not configured
("also how a launcher discovers that this server does not offer logins",
`src/routes/mod.rs`), and such a server cannot require a login
(`DISCORD_REQUIRED` refuses to start without Discord, `src/config.rs`).
Servers that offer sign-in keep the upstream behaviour unchanged.

- `segagenesisrecomp/external/recomp-net/src/auth/rnet_auth.c`
  (branch `nhl94-no-signin` off `nhl94-spectators`): `job_probe` + a
  `rnet_account_pump` branch probe once per lobby host when no device key is
  stored (state WAITING for that round trip); 503 sets "sign-in unavailable".
  A new host re-probes (`rnet_account_init`).
- `recomp-ui/src/netplay/recomp_netplay_host.c` (branch `nhl94-no-signin` off
  `26d9e4f`): `cb_connect` points the account client at the lobby, waits up
  to 5 s for the probe/redemption, refuses with `login_required` only when
  sign-in is offered and missing; a guest gets the launcher's player name, the
  stored name, or `Player-xxxx`.
- `recomp-ui/src/common/backends/imgui/launcher_imgui.cpp`: the ONLINE card
  enters the lobby browser directly when sign-in is not offered, and the
  browser only redirects to the sign-in page when it is.
- `segagenesisrecomp/runner/netplay/genesis_host_lobby.c` (headless selftest
  only): `host_relay_ready()` no longer waits for an advertised port in a
  host-relay-over-ICE room, which the server forces on every online room.

Verified against the real lobby: two mac-ice processes
(`GENESIS_LOBBY_SELFTEST=host|guest`, `NHL94_SEATS=home:C,away:C`, 1800
ticks): room created/joined as guests, `transport=ice`, boot digest agreed,
6/6 TIMELINE digests identical, desyncs=0; server `/stats` counted 1 start
for `nhl_hockey_94` with 2 players.

## Not verified here

- No Wine on this Mac (the Homebrew `wine-stable`/`wine@devel`/`wine@staging`
  casks are disabled since 2026-09-01 for failing Gatekeeper), so the Windows
  exe has not been run; the first real check is the in-match boot digest and
  timeline digests against the Mac.
- `docs/NETPLAY.md` (engine) lists cross-compiler UB in the generated C
  (MSVC/gcc vs clang) as unchecked; a GCC-built Windows peer against a
  clang-built Mac peer is exactly that case. The rollback driver detects a
  divergence (boot digest refusal or a reported fork); it cannot fix one.

See docs/MAC_BUILD.md for the portable macOS build (macOS 11+, universal, no Homebrew) that plays against this Windows build.
