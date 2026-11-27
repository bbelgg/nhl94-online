#!/bin/bash
# package.sh [--portable] <mac build dir> <out.zip> -- the macOS online zip.
# Default mode: Apple Silicon build against Homebrew (bundles its dylibs).
#
# The game binary links Homebrew dylibs (sdl2-compat, which dlopens SDL3 from
# "@loader_path/libSDL3.dylib"; freetype; harfbuzz) and their dependencies.
# This script copies every non-system dylib next to the binary, rewrites all
# load commands to @executable_path / @loader_path, drops the Homebrew rpath,
# ad-hoc signs everything (required on Apple Silicon after install_name_tool),
# and zips it with the engine's allowlist packager
# (segagenesisrecomp/tools/package_release.py: refuses ROMs, saves, dumps, logs).
#
# The bundled Homebrew dylibs carry their own minimum macOS (27.x when this was
# written), so the package runs on Apple Silicon + that macOS or newer only.
#
# --portable <build dir> <out.zip>: for the self-contained universal build
# (docs/MAC_BUILD.md: macOS 11.0+, arm64 + x86_64, SDL2 linked statically, no
# FreeType/HarfBuzz). Nothing is bundled: the script REFUSES if the binary
# loads anything outside /usr/lib and /System, or is not universal / 11.0.
# Env: SDL2_SRC = the SDL2-<ver> source dir the static SDL2 was built from
# (for its LICENSE.txt).
set -euo pipefail
PORTABLE=0
if [ "${1:-}" = "--portable" ]; then PORTABLE=1; shift; fi
BUILD="$(cd "${1:?mac build dir}" && pwd)"
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
BREW="$(brew --prefix 2>/dev/null || echo /nonexistent)"

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/nhl94-mac-stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
cp "$BUILD/NHL94Recomp" "$STAGE/NHL94Recomp"
chmod u+w "$STAGE/NHL94Recomp"

if [ "$PORTABLE" = 1 ]; then
    # ---- portable: verify, sign, nothing to bundle ---------------------------
    bad="$(otool -L "$STAGE/NHL94Recomp" | awk '$NF !~ /:$/ && $1 !~ /^(\/usr\/lib\/|\/System\/)/ {print $1}')"
    [ -z "$bad" ] || { echo "portable build loads non-system libraries:"; echo "$bad"; exit 1; }
    archs="$(lipo -archs "$STAGE/NHL94Recomp")"
    case " $archs " in *" arm64 "*) ;; *) echo "not arm64: $archs"; exit 1;; esac
    case " $archs " in *" x86_64 "*) ;; *) echo "not x86_64: $archs"; exit 1;; esac
    vtool -show-build "$STAGE/NHL94Recomp" | awk '/minos/ && $2 != "11.0" {bad=1} END {exit bad}' \
        || { echo "minos is not 11.0"; vtool -show-build "$STAGE/NHL94Recomp"; exit 1; }
    codesign --force --sign - "$STAGE/NHL94Recomp"
    echo "portable: $archs, minos 11.0, system libraries only"
else
# ---- collect non-system dylibs (recursively) -------------------------------
python3 -I - "$STAGE" "$BREW" <<'PY'
import os, shutil, subprocess, sys
stage, brew = sys.argv[1], sys.argv[2]
exe = os.path.join(stage, "NHL94Recomp")

def load_cmds(p):
    # Dependencies only: for a dylib, otool -L lists its own install name
    # (LC_ID_DYLIB) first; otool -D gives that id so it can be dropped.
    out = subprocess.run(["otool", "-L", p], capture_output=True, text=True, check=True).stdout
    deps = [l.split()[0] for l in out.splitlines()[1:]]
    ids = subprocess.run(["otool", "-D", p], capture_output=True, text=True).stdout.splitlines()[1:]
    own = {i.strip() for i in ids if i.strip()}
    return [d for d in deps if d not in own]

def is_system(d):
    return d.startswith("/usr/lib/") or d.startswith("/System/")

def resolve(d, frm):
    if d.startswith("@rpath/"):
        for base in (os.path.join(brew, "lib"), os.path.dirname(frm)):
            c = os.path.join(base, d[len("@rpath/"):])
            if os.path.exists(c):
                return c
        return None
    if d.startswith("@loader_path/"):
        return os.path.normpath(os.path.join(os.path.dirname(frm), d[len("@loader_path/"):]))
    return d

def run(*a):
    subprocess.run(list(a), check=True, capture_output=True)

bundled = {}   # basename -> staged path
# sdl2-compat loads SDL3 at runtime from @loader_path/libSDL3.dylib
extra = {"libSDL3.dylib": os.path.join(brew, "opt/sdl3/lib/libSDL3.0.dylib")}
queue = [(exe, exe)]
for name, src in extra.items():
    dst = os.path.join(stage, name)
    shutil.copyfile(os.path.realpath(src), dst); os.chmod(dst, 0o644)
    bundled[name] = dst
    queue.append((dst, os.path.realpath(src)))
while queue:
    staged, original = queue.pop()
    for d in load_cmds(staged):
        if is_system(d):
            continue
        name = os.path.basename(d)
        if name not in bundled:
            src = resolve(d, original)
            if not src or not os.path.exists(src):
                sys.exit(f"cannot resolve {d} (from {original})")
            dst = os.path.join(stage, name)
            shutil.copyfile(os.path.realpath(src), dst); os.chmod(dst, 0o644)
            bundled[name] = dst
            queue.append((dst, os.path.realpath(src)))
        new = ("@executable_path/" if staged == exe else "@loader_path/") + name
        if d != new and os.path.basename(d) != os.path.basename(staged):
            run("install_name_tool", "-change", d, new, staged)
for name, p in bundled.items():
    run("install_name_tool", "-id", "@loader_path/" + name, p)
# drop rpaths that point into Homebrew / the build machine
for p in [exe] + list(bundled.values()):
    out = subprocess.run(["otool", "-l", p], capture_output=True, text=True).stdout.splitlines()
    for i, l in enumerate(out):
        if "LC_RPATH" in l:
            path = out[i + 2].split()[1]
            if not path.startswith("@"):
                run("install_name_tool", "-delete_rpath", path, p)
            elif ".." in path:
                run("install_name_tool", "-delete_rpath", path, p)
# ad-hoc signature (Apple Silicon refuses modified, unsigned Mach-Os)
for p in list(bundled.values()) + [exe]:
    run("codesign", "--force", "--sign", "-", p)
# no load command may point outside the package any more
for p in [exe] + list(bundled.values()):
    for d in load_cmds(p):
        if not (is_system(d) or d.startswith("@executable_path/") or d.startswith("@loader_path/")):
            sys.exit(f"{os.path.basename(p)} still loads {d}")
print("bundled:", " ".join(sorted(bundled)))
PY
fi

# ---- licenses ---------------------------------------------------------------
L="$STAGE/licenses"
mkdir -p "$L"
lic() { [ -f "$1" ] && cp "$1" "$L/$2"; }
if [ "$PORTABLE" = 1 ]; then
    cp "$HERE/README-portable.txt"          "$STAGE/README.txt"
else
    cp "$HERE/README.txt"                   "$STAGE/README.txt"
fi
# The README names the lobby this build was configured with (NHL94_LOBBY_URL).
LOBBY_URL=$(sed -n 's/^NHL94_LOBBY_URL:STRING=//p' "$BUILD/CMakeCache.txt")
[ -n "$LOBBY_URL" ] || LOBBY_URL="recomp-net's default lobby (ws://netplay.retcomm.net:8765)"
sed -i.bak "s|@NHL94_LOBBY_URL@|$LOBBY_URL|" "$STAGE/README.txt" && rm -f "$STAGE/README.txt.bak"
cp "$HERE/NHL94.command"                    "$STAGE/NHL94.command"
chmod +x "$STAGE/NHL94.command"
lic "$ENGINE/LICENSE.md"                    segagenesisrecomp-LICENSE.md
lic "$ENGINE/PERMISSIONS.md"                segagenesisrecomp-PERMISSIONS.md
lic "$REPO/recomp-ui/LICENSE"               recomp-ui-LICENSE.txt
lic "$REPO/recomp-ui/src/third_party/imgui/LICENSE.txt" dear-imgui-LICENSE.txt
lic "$REPO/recomp-ui/assets/common/fonts/NOTICE.md"     recomp-ui-fonts-NOTICE.md
lic "$REPO/recomp-ui/assets/common/img/NOTICE.md"       recomp-ui-img-NOTICE.md
lic "$ENGINE/external/recomp-net/LICENSE"   recomp-net-LICENSE.txt
lic "$ENGINE/external/rbengine/LICENSE"     rbengine-LICENSE.txt
lic "$BUILD/_deps/libjuice-src/LICENSE"     libjuice-LICENSE-MPL-2.0.txt
lic "$ENGINE/runner/external/ymfm/LICENSE"              ymfm-LICENSE.txt
lic "$ENGINE/runner/external/superzazu/LICENSE"         superzazu-z80-LICENSE.txt
lic "$ENGINE/runner/external/clowncommon/LICENCE.txt"   clowncommon-LICENCE.txt
lic "$ENGINE/runner/external/minicoro/LICENSE"          minicoro-LICENSE.txt
lic "$ENGINE/runner/external/xxhash/LICENSE.txt"        xxhash-LICENSE.txt
if [ "$PORTABLE" = 1 ]; then
cp "${SDL2_SRC:?set SDL2_SRC to the SDL2-<ver> source dir}/LICENSE.txt" "$L/SDL2-LICENSE.txt"
cat > "$L/README-licenses.txt" <<'TXT'
Notices for components linked into NHL94Recomp or shipped beside it.
  SDL2 2.28.5 (static, unmodified release)  zlib       SDL2-LICENSE.txt
  libjuice v1.7.2 (static, ICE)             MPL-2.0    unmodified source:
      https://github.com/paullouisageneau/libjuice/tree/v1.7.2
  recomp-ui launcher, Dear ImGui            MIT
  stb_image/stb_truetype (public domain), tinyfiledialogs (zlib): notices in
      their source headers in recomp-ui/src/third_party
  launcher fonts/images                     OFL-1.1 / CC BY-SA 4.0
  recomp-net, rbengine                      MIT
  ymfm BSD-3, superzazu Z80 MIT, clowncommon ISC, minicoro, xxHash BSD-2
  segagenesisrecomp engine                  PolyForm Noncommercial 1.0.0 (+ PERMISSIONS)
TXT
else
lic "$BREW/opt/sdl2-compat/LICENSE.txt"     sdl2-compat-LICENSE.txt
lic "$BREW/opt/sdl3/LICENSE.txt"            SDL3-LICENSE.txt
lic "$BREW/opt/freetype/LICENSE.TXT"        freetype-LICENSE.txt
lic "$BREW/opt/harfbuzz/COPYING"            harfbuzz-COPYING.txt
lic "$BREW/opt/gettext/COPYING"             gettext-libintl-COPYING.txt
lic "$BREW/opt/pcre2/LICENCE.md"            pcre2-LICENCE.md
lic "$BREW/opt/libpng/LICENSE"              libpng-LICENSE.txt
lic "$BREW/opt/brotli/LICENSE"              brotli-LICENSE.txt
lic "$BREW/opt/graphite2/LICENSE"           graphite2-LICENSE.txt
for g in "$BREW"/opt/glib/LGPL-2.1-or-later.txt "$BREW"/opt/glib/share/doc/glib*/COPYING "$BREW"/opt/glib/COPYING; do
    [ -f "$g" ] && { cp "$g" "$L/glib-LGPL-2.1-or-later.txt"; break; }
done
cat > "$L/README-licenses.txt" <<'TXT'
Notices for components linked into NHL94Recomp or shipped beside it.
  sdl2-compat (libSDL2-2.0.0.dylib), SDL3 (libSDL3.dylib)   zlib
  libjuice (static, ICE)                    MPL-2.0    unmodified source:
      https://github.com/paullouisageneau/libjuice
  recomp-ui launcher, Dear ImGui            MIT
  stb_image/stb_truetype (public domain), tinyfiledialogs (zlib): notices in
      their source headers in recomp-ui/src/third_party
  launcher fonts/images                     OFL-1.1 / CC BY-SA 4.0
  recomp-net, rbengine                      MIT
  ymfm BSD-3, superzazu Z80 MIT, clowncommon ISC, minicoro, xxHash BSD-2
  FreeType (FTL), HarfBuzz (MIT), GLib + gettext libintl (LGPL-2.1-or-later,
      dynamically linked, unmodified Homebrew builds: replaceable .dylib
      files in this folder), PCRE2 (BSD), libpng (libpng), Brotli (MIT),
      Graphite2 (LGPL-2.1 / MPL)
  segagenesisrecomp engine                  PolyForm Noncommercial 1.0.0 (+ PERMISSIONS)
TXT
fi

# ---- zip through the allowlist packager ------------------------------------
EXTRA=("$REPO/release/settings.ini" "$STAGE/NHL94.command")
for f in "$STAGE"/*.dylib; do [ -e "$f" ] && EXTRA+=("$f"); done
mkdir -p "$(dirname "$OUT")"
python3 "$ENGINE/tools/package_release.py" \
    --exe       "$STAGE/NHL94Recomp" \
    --extra     "${EXTRA[@]}" \
    --asset-dir "$BUILD/assets" "$L" \
    --license   "$REPO/LICENSE" \
    --notices   "$ENGINE/THIRD-PARTY-LICENSES.md" \
    --readme    "$STAGE/README.txt" \
    --out       "$OUT"
echo "package: $OUT"
