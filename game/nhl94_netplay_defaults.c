/*
 * nhl94_netplay_defaults.c -- build-time default lobby server for this game.
 *
 * recomp-net resolves the lobby URL as: the URL saved in the launcher's
 * network settings (saves/netplay/network settings), else RNET_LOBBY_URL /
 * GENESIS_NET_LOBBY_URL from the environment, else its own built-in default.
 * CMake option NHL94_LOBBY_URL compiles this file in and seeds RNET_LOBBY_URL
 * at process start ONLY when neither variable is already set, so an explicit
 * environment value and the launcher's saved setting both still win, and the
 * engine / recomp-net defaults stay untouched.
 */
#include <stdlib.h>

#ifndef NHL94_LOBBY_URL
#error "compile with -DNHL94_LOBBY_URL=\"ws://host:port\""
#endif

static void nhl94_seed_lobby_url(void)
{
    const char *a = getenv("RNET_LOBBY_URL");
    const char *b = getenv("GENESIS_NET_LOBBY_URL");
    if ((a && a[0]) || (b && b[0]))
        return;
#ifdef _WIN32
    _putenv_s("RNET_LOBBY_URL", NHL94_LOBBY_URL);
#else
    setenv("RNET_LOBBY_URL", NHL94_LOBBY_URL, 0);
#endif
}

#if defined(_MSC_VER)
/* MSVC: run before main via the CRT initializer table. */
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void (*nhl94_seed_lobby_url_init)(void) = nhl94_seed_lobby_url;
#pragma comment(linker, "/include:nhl94_seed_lobby_url_init")
#else
__attribute__((constructor)) static void nhl94_seed_lobby_url_ctor(void)
{
    nhl94_seed_lobby_url();
}
#endif
