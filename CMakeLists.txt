cmake_minimum_required(VERSION 3.16)
project(NHL94Recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# ---------------------------------------------------------------------------
# Engine location.
#
# The pinned engine lives in the segagenesisrecomp/ submodule. For local
# engine development, point GENESIS_RECOMP_ROOT at a sibling checkout instead
# (the _build_native.bat wrapper does this automatically when one exists).
# ---------------------------------------------------------------------------
set(GENESIS_RECOMP_ROOT "" CACHE PATH
    "Override: absolute path to a segagenesisrecomp checkout (default: submodule)")
if(GENESIS_RECOMP_ROOT)
    get_filename_component(RECOMP_ROOT "${GENESIS_RECOMP_ROOT}" ABSOLUTE)
else()
    set(RECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/segagenesisrecomp")
endif()
if(NOT EXISTS "${RECOMP_ROOT}/recompiler/CMakeLists.txt")
    message(FATAL_ERROR
        "segagenesisrecomp engine not found at:\n    ${RECOMP_ROOT}\n"
        "Run `git submodule update --init --recursive`, or configure with "
        "-DGENESIS_RECOMP_ROOT=<path to a segagenesisrecomp checkout>.")
endif()
if(NOT EXISTS "${RECOMP_ROOT}/external/m68k-recomp-core/cmake/M68kRecompCore.cmake")
    message(FATAL_ERROR
        "The engine's external/m68k-recomp-core submodule is not checked out under\n"
        "    ${RECOMP_ROOT}\nRun `git submodule update --init --recursive` there.")
endif()
message(STATUS "NHL94Recomp: engine root = ${RECOMP_ROOT}")

set(RUNNER_ROOT "${RECOMP_ROOT}/runner")
set(GAME_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/game")

# ---------------------------------------------------------------------------
# Build options.
# ---------------------------------------------------------------------------
option(GEN_ENABLE_TRACE
    "Compile the TCP command server + history rings (dev builds). OFF = shipping build." ON)
option(GENESIS_REVERSE_DEBUG
    "Emit reverse-debug instrumentation into the generated C" OFF)
set(GENESIS_DEBUG_PORT 4380 CACHE STRING "Compile-time default TCP debug port")
option(NHL94_COPY_ROM_TO_BUILD
    "Windows: copy game/NHL94.bin next to the built exe (dev convenience)" ON)
# Default lobby: -DNHL94_LOBBY_URL=... > the NHL94_LOBBY_URL environment variable
# > NHL94_LOBBY_URL_DEFAULT from local.cmake (git-ignored; see
# local.cmake.example) > empty (recomp-net's own default lobby). Read when a
# build directory is first configured; later it is the cached value.
include("${CMAKE_CURRENT_SOURCE_DIR}/local.cmake" OPTIONAL)
if(DEFINED ENV{NHL94_LOBBY_URL} AND NOT "$ENV{NHL94_LOBBY_URL}" STREQUAL "")
    set(NHL94_LOBBY_URL_DEFAULT "$ENV{NHL94_LOBBY_URL}")
endif()
if(NOT DEFINED NHL94_LOBBY_URL_DEFAULT)
    set(NHL94_LOBBY_URL_DEFAULT "")
endif()
set(NHL94_LOBBY_URL "${NHL94_LOBBY_URL_DEFAULT}" CACHE STRING
    "Netplay: default lobby server (ws://host:port) when neither RNET_LOBBY_URL nor GENESIS_NET_LOBBY_URL is set| empty = recomp-net's default")
option(NHL94_NETPLAY_SOURCE_IDENTITY
    "Netplay: identify the build by a hash of its sources + generated C instead of the exe bytes, so Windows/macOS/Linux builds of one tree can play each other" ON)
set(NHL94_SDL2_MINGW_PREFIX "" CACHE PATH
    "MinGW builds: the x86_64-w64-mingw32 directory of the official SDL2-devel-<ver>-mingw package")

# ---------------------------------------------------------------------------
# Generated 68K -> C translation. Regenerated from game/NHL94.bin + game/game.toml
# by the engine's recompiler whenever any input changes. Never edit the output.
# ---------------------------------------------------------------------------
include("${RECOMP_ROOT}/cmake/GenesisRecompGenerated.cmake")
genesisrecomp_add_generated_sources(NHL94_GEN nhl94 "${GAME_DIR}" NHL94.bin
    REVERSE_DEBUG ${GENESIS_REVERSE_DEBUG})

# ---------------------------------------------------------------------------
# The build picker's roster table (names + ratings), generated from the
# player's own ROM into the build directory -- never checked in.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(NHL94_ROSTER_DIR "${CMAKE_CURRENT_BINARY_DIR}/nhl94_roster")
add_custom_command(
    OUTPUT "${NHL94_ROSTER_DIR}/nhl94_roster_table.c" "${NHL94_ROSTER_DIR}/nhl94_roster_table.h"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/builds/gen_roster.py"
            --rom "${GAME_DIR}/NHL94.bin" --out "${NHL94_ROSTER_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/builds/gen_roster.py" "${GAME_DIR}/NHL94.bin"
    COMMENT "Generating the build picker's roster table from game/NHL94.bin"
    VERBATIM)

# ---------------------------------------------------------------------------
# Shared clean-room runner (engine-owned). One backend: ours.
# ---------------------------------------------------------------------------
set(RUNNER_SOURCES
    "${RUNNER_ROOT}/main.c"
    "${RUNNER_ROOT}/glue.c"
    "${RUNNER_ROOT}/app_config.c"
    "${RUNNER_ROOT}/audio.c"
    "${RUNNER_ROOT}/chip_trace.c"
    "${RUNNER_ROOT}/crash_report.c"
    "${RUNNER_ROOT}/fiber_compat.c"
    "${RUNNER_ROOT}/frame_snapshots.c"
    "${RUNNER_ROOT}/game_cycles.c"
    "${RUNNER_ROOT}/gamepad.c"
    "${RUNNER_ROOT}/input_map.c"
    "${RUNNER_ROOT}/input_script.c"
    "${RUNNER_ROOT}/m68k_interp.c"
    "${RUNNER_ROOT}/reverse_debug.c"
    # State hashing is used by the always-on benchmark/state paths in main.c|
    # the lockstep cosim server (cosim.c, cosim_visible.c) is GENESIS_COSIM-only.
    "${RUNNER_ROOT}/cosim_state.c"
    # z80_recomp.c is the opt-in Z80 static-recomp experiment (GENESIS_Z80_RECOMP)|
    # the default build runs the superzazu Z80 interpreter.
    "${RUNNER_ROOT}/video/genesis_bus.c"
    "${RUNNER_ROOT}/video/genesis_machine.c"
    "${RUNNER_ROOT}/video/genesis_vdp.c"
    "${RUNNER_ROOT}/video/color_lut.c"
    "${RUNNER_ROOT}/audio/mixer.c"
    "${RUNNER_ROOT}/audio/event_queue.c"
    "${RUNNER_ROOT}/audio/observability.c"
    "${RUNNER_ROOT}/audio/sn76489.c"
    "${RUNNER_ROOT}/audio/ym2612_ymfm.cpp"
    "${RUNNER_ROOT}/audio/audio_shadow.c"
    "${RUNNER_ROOT}/audio/fm_shadow.cpp"
    # Vendored third-party (see THIRD-PARTY-LICENSES.md in the engine).
    "${RUNNER_ROOT}/external/superzazu/z80.c"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_opn.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_ssg.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_adpcm.cpp"
    # The Tier-3 interpreter decodes with the same clean-room decoder the
    # recompiler uses| the ROM view comes from the recompiler's parser.
    "${RECOMP_ROOT}/external/m68k-recomp-core/common/m68k_decoder.c"
    "${RECOMP_ROOT}/recompiler/src/rom_parser.c"
)
if(GEN_ENABLE_TRACE)
    list(APPEND RUNNER_SOURCES "${RUNNER_ROOT}/cmd_server.c")
    # Rare-event dev prints ([DMA-STALL], [VINT-MASK], [IRQ-DEBT]) and the
    # chip/sound trace commands. Off in shipping builds.
    add_compile_definitions(GEN_DEV_TRACE=1)
else()
    list(APPEND RUNNER_SOURCES "${RUNNER_ROOT}/cmd_server_stub.c")
endif()

add_executable(NHL94Recomp
    ${RUNNER_SOURCES}
    ${NHL94_GEN}
    "${NHL94_GEN_CYCLES}"
    "${GAME_DIR}/nhl94_spec.c"
    "${GAME_DIR}/nhl94_view.c"
    "${GAME_DIR}/nhl94_seats.c"
    "${GAME_DIR}/nhl94_format.c"
    "${GAME_DIR}/nhl94_mode.c"
    "${GAME_DIR}/nhl94_tags.c"
    "${GAME_DIR}/nhl94_tagnames.c"
    "${GAME_DIR}/nhl94_aim.c"
    "${GAME_DIR}/nhl94_builds.c"
    "${NHL94_ROSTER_DIR}/nhl94_roster_table.c"
)
add_dependencies(NHL94Recomp ${NHL94_GEN_TARGET})

target_include_directories(NHL94Recomp PRIVATE
    "${GAME_DIR}"
    "${NHL94_ROSTER_DIR}"
    "${NHL94_GEN_DIR}"
    "${RUNNER_ROOT}"
    "${RUNNER_ROOT}/include"
    "${RUNNER_ROOT}/video"
    "${RUNNER_ROOT}/audio"
    "${RUNNER_ROOT}/external"
    "${RUNNER_ROOT}/external/clowncommon"
    "${RUNNER_ROOT}/external/superzazu"
    "${RUNNER_ROOT}/external/ymfm/src"
    "${RECOMP_ROOT}/recompiler/src"
    "${RECOMP_ROOT}/external/m68k-recomp-core/common"
    "${RECOMP_ROOT}/external/m68k-recomp-core/profiles/genesis"
)

target_compile_definitions(NHL94Recomp PRIVATE
    DEFAULT_DEBUG_PORT=${GENESIS_DEBUG_PORT}
    GENESIS_GAME_VERSION="dev"
)

# ---------------------------------------------------------------------------
# SDL2 — bundled with the engine on Windows| system package elsewhere.
# ---------------------------------------------------------------------------
if(MINGW)
    # Cross (or native) MinGW-w64 build. The engine's bundled SDL2 is MSVC-only|
    # use the official SDL2-devel-<ver>-mingw package. See docs/WINDOWS_BUILD.md.
    if(NOT NHL94_SDL2_MINGW_PREFIX OR NOT EXISTS "${NHL94_SDL2_MINGW_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "MinGW build: set -DNHL94_SDL2_MINGW_PREFIX=<SDL2-x.y.z>/x86_64-w64-mingw32 "
            "(from SDL2-devel-x.y.z-mingw.tar.gz).")
    endif()
    # Both spellings are used: <SDL.h> and <SDL2/SDL.h>.
    target_include_directories(NHL94Recomp PRIVATE
        "${NHL94_SDL2_MINGW_PREFIX}/include/SDL2" "${NHL94_SDL2_MINGW_PREFIX}/include")
    # Import library by full path (not -lSDL2) so -static below cannot pick
    # the static libSDL2.a| SDL2.dll ships next to the exe.
    target_link_libraries(NHL94Recomp PRIVATE
        "${NHL94_SDL2_MINGW_PREFIX}/lib/libSDL2.dll.a" ws2_32)
    # Fold libgcc/libstdc++/winpthread into the exe: the package then needs
    # only SDL2.dll besides Windows system DLLs.
    target_link_options(NHL94Recomp PRIVATE -static -static-libgcc -static-libstdc++)
    # Keep this machine's build/source paths out of __FILE__ strings
    # (libjuice and other subprojects added below inherit this).
    add_compile_options("-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
                        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=src")
    add_custom_command(TARGET NHL94Recomp POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${NHL94_SDL2_MINGW_PREFIX}/bin/SDL2.dll" "$<TARGET_FILE_DIR:NHL94Recomp>"
        VERBATIM)
elseif(WIN32)
    set(SDL2_BUNDLED "${RUNNER_ROOT}/external/SDL2/SDL2-2.28.5")
    target_include_directories(NHL94Recomp PRIVATE "${SDL2_BUNDLED}/include")
    target_link_libraries(NHL94Recomp PRIVATE
        "${SDL2_BUNDLED}/lib/x64/SDL2.lib"
        "${SDL2_BUNDLED}/lib/x64/SDL2main.lib"
        ws2_32)
    add_custom_command(TARGET NHL94Recomp POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${SDL2_BUNDLED}/lib/x64/SDL2.dll" "$<TARGET_FILE_DIR:NHL94Recomp>"
        VERBATIM)
    if(NHL94_COPY_ROM_TO_BUILD)
        add_custom_command(TARGET NHL94Recomp POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${GAME_DIR}/NHL94.bin" "$<TARGET_FILE_DIR:NHL94Recomp>/nhl94.bin"
            VERBATIM)
    endif()
    if(GEN_ENABLE_TRACE)
        # The TCP debug server only arms when debug.ini sits next to the exe.
        add_custom_command(TARGET NHL94Recomp POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/debug.ini" "$<TARGET_FILE_DIR:NHL94Recomp>"
            VERBATIM)
    endif()
else()
    find_package(SDL2 REQUIRED)
    target_include_directories(NHL94Recomp PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(NHL94Recomp PRIVATE ${SDL2_LIBRARIES} m pthread)
endif()

if(MSVC)
    target_compile_options(NHL94Recomp PRIVATE /W3 /WX- /utf-8)
    target_compile_definitions(NHL94Recomp PRIVATE _CRT_SECURE_NO_WARNINGS)
    # 32 generated shards can be large| MSVC needs /bigobj for some of them.
    target_compile_options(NHL94Recomp PRIVATE /bigobj)
endif()

# ---------------------------------------------------------------------------
# Shared recomp-ui (Dear ImGui) pre-boot launcher (recomp-ui/ submodule).
# Defines RECOMP_LAUNCHER, which un-gates the launcher seam in the runner's
# main.c, and stages assets/ next to the exe. --no-launcher still boots the
# ROM directly (scripts, benchmarks).
# ---------------------------------------------------------------------------
# The launcher's Mods page hosts NHL 94's Control mode setting (Classic /
# Position lock, game/nhl94_mode.c through GameSpec.mods), so this game turns
# recomp-ui's Mods view on (default OFF in recomp-ui).
set(RECOMP_UI_ENABLE_MODS ON CACHE BOOL "Enable the recomp-ui Mods view (NHL 94: Control mode)")
include("${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui/recomp_ui.cmake")
recomp_target_launcher_ui(NHL94Recomp CONSOLE genesis)

# ---------------------------------------------------------------------------
# Rollback netplay (opt-in). Off by default| configure with
# -DGENESISRECOMP_NETPLAY=ON to compile the engine's recomp-net transport,
# rbengine snapshot ring and recomp-ui lobby into the runner. The peer
# handshake uses GENESIS_GAME_VERSION (defined above), so every peer must
# run the same build.
# ---------------------------------------------------------------------------
include("${RECOMP_ROOT}/cmake/GenesisRecompNetplay.cmake")
genesisrecomp_enable_netplay(NHL94Recomp)

if(GENESISRECOMP_NETPLAY)
    # Default lobby server baked into this build (env / saved setting win).
    if(NHL94_LOBBY_URL)
        target_sources(NHL94Recomp PRIVATE "${GAME_DIR}/nhl94_netplay_defaults.c")
        set_source_files_properties("${GAME_DIR}/nhl94_netplay_defaults.c" PROPERTIES
            COMPILE_DEFINITIONS "NHL94_LOBBY_URL=\"${NHL94_LOBBY_URL}\"")
        message(STATUS "NHL94Recomp: default lobby ${NHL94_LOBBY_URL}")
    endif()

    # Platform-independent netplay identity (cmake/NetplaySourceIdentity.cmake):
    # hashed from the sources + generated C before every build, so a macOS and
    # a Windows build of the same tree get the same lobby version / rollback
    # build fingerprint, and any source difference still separates them.
    if(NHL94_NETPLAY_SOURCE_IDENTITY)
        set(_np_id_dir "${CMAKE_CURRENT_BINARY_DIR}/netplay_source_identity")
        set(_np_id_hdr "${_np_id_dir}/genesis_netplay_source_identity.h")
        # Normalized (ON/1/TRUE all hash the same).
        set(_np_id_ice 0)
        set(_np_id_trace 0)
        set(_np_id_rdbg 0)
        if(RNET_ENABLE_ICE)
            set(_np_id_ice 1)
        endif()
        if(GEN_ENABLE_TRACE)
            set(_np_id_trace 1)
        endif()
        if(GENESIS_REVERSE_DEBUG)
            set(_np_id_rdbg 1)
        endif()
        add_custom_target(NHL94Recomp_netplay_identity
            COMMAND "${CMAKE_COMMAND}"
                "-DOUT=${_np_id_hdr}"
                "-DBASE=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DROOTS=${GAME_DIR}|${RUNNER_ROOT}|${RECOMP_ROOT}/external/m68k-recomp-core|${RECOMP_ROOT}/recompiler/src|${RECOMP_ROOT}/external/recomp-net/src|${RECOMP_ROOT}/external/recomp-net/include|${RECOMP_ROOT}/external/rbengine|${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui/src|${RECOMP_ROOT}/cmake|${CMAKE_CURRENT_SOURCE_DIR}/cmake"
                "-DFILES=${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt|${CMAKE_CURRENT_SOURCE_DIR}/recomp-ui/recomp_ui.cmake"
                "-DGEN_DIR=${NHL94_GEN_DIR}"
                "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
                "-DOPTIONS=GEN_ENABLE_TRACE=${_np_id_trace}|GENESIS_REVERSE_DEBUG=${_np_id_rdbg}|ICE=${_np_id_ice}|GAME_VERSION=dev"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/NetplaySourceIdentity.cmake"
            BYPRODUCTS "${_np_id_hdr}"
            COMMENT "Hashing sources for the netplay identity"
            VERBATIM)
        add_dependencies(NHL94Recomp_netplay_identity ${NHL94_GEN_TARGET})
        add_dependencies(NHL94Recomp NHL94Recomp_netplay_identity)
        target_include_directories(NHL94Recomp PRIVATE "${_np_id_dir}")
        target_compile_definitions(NHL94Recomp PRIVATE GENESIS_NETPLAY_SOURCE_IDENTITY=1)
    endif()
endif()
