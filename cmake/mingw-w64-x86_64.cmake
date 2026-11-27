# CMake toolchain: cross-compile NHL94Recomp for Windows x64 with MinGW-w64
# (Homebrew `brew install mingw-w64` on macOS, or the distro package on Linux).
# See docs/WINDOWS_BUILD.md. The 68K->C generator must run on the BUILD host,
# so configure with -DGENESIS_RECOMPILER_EXE=<host GenesisRecomp>.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(_mingw_triple x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${_mingw_triple}-gcc)
set(CMAKE_CXX_COMPILER ${_mingw_triple}-g++)
set(CMAKE_RC_COMPILER  ${_mingw_triple}-windres)
set(CMAKE_AR           ${_mingw_triple}-ar CACHE FILEPATH "")
set(CMAKE_RANLIB       ${_mingw_triple}-ranlib CACHE FILEPATH "")

# Search headers/libs only in the target sysroot (+ explicit prefixes such as
# the SDL2 mingw package); host programs (git, python) come from the host.
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-sysroot
                OUTPUT_VARIABLE _mingw_sysroot OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(CMAKE_FIND_ROOT_PATH ${_mingw_sysroot})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
