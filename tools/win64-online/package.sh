#!/bin/bash
# package.sh <win64 build dir> <out.zip> -- the Windows online zip, built with
# the engine's allowlist packager (segagenesisrecomp/tools/package_release.py,
# which refuses any ROM / save / dump / log). See docs/WINDOWS_BUILD.md.
#
# Env: SDL2_MINGW_ROOT = the extracted SDL2-<ver> directory of the
#      SDL2-devel-<ver>-mingw package (for its COPYING.txt).
set -euo pipefail
BUILD="$(cd "${1:?build dir}" && pwd)"
# Release rule (segagenesisrecomp RELEASING.md): no developer diagnostics in a
# shipped build. Refuse a build configured with GEN_ENABLE_TRACE=ON (it compiles
# the TCP command server, history rings and GEN_DEV_TRACE prints).
if ! grep -q '^GEN_ENABLE_TRACE:BOOL=OFF$' "$BUILD/CMakeCache.txt" 2>/dev/null; then
    echo "package: refusing $BUILD -- configure it with -DGEN_ENABLE_TRACE=OFF (release build)" >&2
    exit 1
fi
OUT="${2:?out.zip}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
ENGINE="$REPO/segagenesisrecomp"
SDL_ROOT="${SDL2_MINGW_ROOT:?set SDL2_MINGW_ROOT to the SDL2-<ver> dir of the mingw package}"
MINGW_PREFIX="$(brew --prefix mingw-w64 2>/dev/null || true)"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/nhl94-win64-stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
L="$STAGE/licenses"
mkdir -p "$L"
cp "$HERE/README.txt"              "$STAGE/README.txt"
# The README names the lobby this build was configured with (NHL94_LOBBY_URL).
LOBBY_URL=$(sed -n 's/^NHL94_LOBBY_URL:STRING=//p' "$BUILD/CMakeCache.txt")
[ -n "$LOBBY_URL" ] || LOBBY_URL="recomp-net's default lobby (ws://netplay.retcomm.net:8765)"
sed -i.bak "s|@NHL94_LOBBY_URL@|$LOBBY_URL|" "$STAGE/README.txt" && rm -f "$STAGE/README.txt.bak"
cp "$ENGINE/LICENSE.md"            "$L/segagenesisrecomp-LICENSE.md"
cp "$ENGINE/PERMISSIONS.md"        "$L/segagenesisrecomp-PERMISSIONS.md"
cp "$REPO/recomp-ui/LICENSE"       "$L/recomp-ui-LICENSE.txt"
cp "$REPO/recomp-ui/src/third_party/imgui/LICENSE.txt" "$L/dear-imgui-LICENSE.txt"
cp "$REPO/recomp-ui/assets/common/fonts/NOTICE.md"     "$L/recomp-ui-fonts-NOTICE.md"
cp "$REPO/recomp-ui/assets/common/img/NOTICE.md"       "$L/recomp-ui-img-NOTICE.md"
cp "$ENGINE/external/recomp-net/LICENSE" "$L/recomp-net-LICENSE.txt"
cp "$ENGINE/external/rbengine/LICENSE"   "$L/rbengine-LICENSE.txt"
cp "$SDL_ROOT/COPYING.txt"               "$L/SDL2-COPYING.txt"
cp "$BUILD/_deps/libjuice-src/LICENSE"   "$L/libjuice-LICENSE-MPL-2.0.txt"
cp "$ENGINE/runner/external/ymfm/LICENSE"          "$L/ymfm-LICENSE.txt"
cp "$ENGINE/runner/external/superzazu/LICENSE"     "$L/superzazu-z80-LICENSE.txt"
cp "$ENGINE/runner/external/clowncommon/LICENCE.txt" "$L/clowncommon-LICENCE.txt"
cp "$ENGINE/runner/external/minicoro/LICENSE"      "$L/minicoro-LICENSE.txt"
cp "$ENGINE/runner/external/xxhash/LICENSE.txt"    "$L/xxhash-LICENSE.txt"
if [ -n "$MINGW_PREFIX" ] && [ -f "$MINGW_PREFIX/COPYING" ]; then
    cp "$MINGW_PREFIX/COPYING" "$L/mingw-w64-COPYING.txt"
fi
cat > "$L/README-licenses.txt" <<'TXT'
Notices for components linked into NHL94Recomp.exe or shipped beside it.
  SDL2 2.28.5 (SDL2.dll)                    zlib       SDL2-COPYING.txt
  libjuice v1.7.2 (static, ICE)             MPL-2.0    libjuice-LICENSE-MPL-2.0.txt
      unmodified source: https://github.com/paullouisageneau/libjuice/tree/v1.7.2
  recomp-ui launcher, Dear ImGui            MIT        recomp-ui-LICENSE.txt, dear-imgui-LICENSE.txt
  stb_image/stb_truetype (public domain), tinyfiledialogs (zlib): notices in
      their source headers in recomp-ui/src/third_party
  launcher fonts/images                     OFL-1.1 / CC BY-SA 4.0  recomp-ui-*-NOTICE.md
  recomp-net, rbengine                      MIT
  ymfm BSD-3, superzazu Z80 MIT, clowncommon ISC, minicoro, xxHash BSD-2
  MinGW-w64 runtime (CRT startup, winpthreads; libgcc/libstdc++ under the
      GCC Runtime Library Exception)        mingw-w64-COPYING.txt
  segagenesisrecomp engine                  PolyForm Noncommercial 1.0.0 (+ PERMISSIONS)
TXT

python3 "$ENGINE/tools/package_release.py" \
    --exe       "$BUILD/NHL94Recomp.exe" \
    --extra     "$BUILD/SDL2.dll" "$REPO/release/settings.ini" \
    --asset-dir "$BUILD/assets" "$L" \
    --license   "$REPO/LICENSE" \
    --notices   "$ENGINE/THIRD-PARTY-LICENSES.md" \
    --readme    "$STAGE/README.txt" \
    --out       "$OUT"
