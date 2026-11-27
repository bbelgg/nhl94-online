# Portable macOS build (macOS 11+, Apple Silicon + Intel)

The plain macOS build links Homebrew dylibs (sdl2-compat/SDL3, FreeType,
HarfBuzz -> GLib ...). Bundling them (`tools/mac-online/package.sh` default
mode) only runs on the macOS those bottles were built for (here 27.x: GLib
imports `pipe2`, which older macOS lacks). The portable build avoids every
runtime dependency outside macOS itself:

- one universal binary (`arm64;x86_64`), deployment target **11.0**;
- SDL2 **2.28.5** (the same SDL2 version as the Windows build), the official
  release source built as a static universal library for 11.0;
- **no FreeType/HarfBuzz**: recomp-ui only uses them for color emoji and
  falls back to its outline glyphs without them (`find_package(Freetype QUIET)`
  in `recomp-ui/recomp_ui.cmake`); disabled with
  `CMAKE_DISABLE_FIND_PACKAGE_Freetype=ON`. No GLib, libpng, brotli, graphite.
- libjuice (ICE) static via FetchContent, as in every build.

No source or hashed build file changes for this: the netplay identity is the
same as the Windows and Homebrew macOS builds of the same tree
(`dev+97103568` at 718c85d).

## 1. SDL2 (static, universal, macOS 11.0)

```sh
B=~/nhl94-online-build
mkdir -p $B/deps/src && cd $B/deps/src
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-2.28.5/SDL2-2.28.5.tar.gz
# sha256 332cb37d0be20cb9541739c61f79bae5a477427d79ae85e352089afdaf6666e4
tar xzf SDL2-2.28.5.tar.gz
cmake -S SDL2-2.28.5 -B sdl2-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
  "-DCMAKE_C_FLAGS=-ffile-prefix-map=$B/deps/src/SDL2-2.28.5=SDL2-2.28.5" \
  -DCMAKE_INSTALL_PREFIX=$B/deps/macos-universal
cmake --build sdl2-build && cmake --install sdl2-build
```

## 2. The game

The 68K->C generator runs on the build host; reuse the native one from the
`mac-ice` build (docs/WINDOWS_BUILD.md, step 1).

```sh
FLAGS="-Werror=unguarded-availability-new -ffile-prefix-map=$B/mac-portable=build -ffile-prefix-map=$HOME/src/nhl94-online/NHL94Recomp=src"
cmake -S ~/src/nhl94-online/NHL94Recomp -B $B/mac-portable -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
  -DGEN_ENABLE_TRACE=OFF -DGENESIS_REVERSE_DEBUG=OFF -DGENESISRECOMP_NETPLAY=ON -DGENESISRECOMP_NET_ICE=ON \
  -DGENESIS_RECOMPILER_EXE=$B/mac-ice/GenesisRecomp \
  -DSDL2_DIR=$B/deps/macos-universal/lib/cmake/SDL2 \
  -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
  -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
  "-DCMAKE_C_FLAGS=$FLAGS" "-DCMAKE_CXX_FLAGS=$FLAGS"
cmake --build $B/mac-portable --target NHL94Recomp
```

`-Werror=unguarded-availability-new` turns any unguarded use of an API newer
than the 11.0 target into a build error (engine, recomp-ui, recomp-net,
libjuice; the build is clean). `-ffile-prefix-map` keeps build paths out of
the binary.

## 3. Check and package

```sh
lipo -archs $B/mac-portable/NHL94Recomp            # x86_64 arm64
vtool -show-build $B/mac-portable/NHL94Recomp      # minos 11.0 for both slices
otool -L $B/mac-portable/NHL94Recomp               # /usr/lib + /System only
SDL2_SRC=$B/deps/src/SDL2-2.28.5 \
  tools/mac-online/package.sh --portable $B/mac-portable $B/dist/NHL94Online-mac-universal-<sha>.zip
```

`package.sh --portable` refuses a binary that loads anything outside
`/usr/lib` and `/System`, is not universal, or is not minos 11.0; it ad-hoc
signs the binary, ships `README-portable.txt` as README.txt, the SDL2 zlib
license and the usual notices, through the engine's allowlist packager.

## Verified (718c85d, this Mac: macOS 27.2, Apple Silicon)

- `x86_64 arm64`, minos 11.0 both slices; `otool -L`: only `/usr/lib` and
  `/System/Library/Frameworks` (GameController, Metal, QuartzCore,
  CoreHaptics, libobjc weak); a `DYLD_PRINT_LIBRARIES` run loads nothing
  outside them.
- No strongly imported symbol is newer than macOS 11.0 according to the SDK
  headers' availability attributes (626 strong imports scanned, 4 heuristic
  hits checked by hand and false: `abort`, `strcasestr`, `NSImage`,
  `NSScreen` are old). `pipe2`, `posix_spawn_file_actions_addchdir_np`,
  `os_sync_*`, `aligned_alloc`, `getentropy` are not imported at all.
- `--no-launcher --benchmark 600` / `3000`: arm64 and x86_64 (Rosetta) give
  `state_fnv1a64` `0294F4D79BB3CAC3` / `AD39B6265B8136CA` (audio
  `0CCB35C74F00F72E` / `7E6C23D58028DD8B`), identical to the Homebrew mac-ice
  build. LAN netplay arm64 host vs x86_64 guest: identity matches, boot
  digest agreed, timelines identical, desyncs=0.
- Not run on a real macOS 11-26 machine or Intel hardware.

Note: with the real SDL2, `SDL_VIDEODRIVER=dummy` cannot create a renderer
(the runner asks for an accelerated one; sdl2-compat was lenient), so
headless runs of this build open a window. Use the Cocoa driver.

## Default lobby server

Netplay builds bake in a default lobby address (`NHL94_LOBBY_URL`). Set it once
per machine: copy `local.cmake.example` to `local.cmake` (git-ignored) and put
your lobby there, or export `NHL94_LOBBY_URL`, or pass `-DNHL94_LOBBY_URL=...`.
Left empty, builds use recomp-net's default lobby. It is read when a build
directory is first configured; the packaged README names the build's lobby.
