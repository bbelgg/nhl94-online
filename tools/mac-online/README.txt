NHL94Recomp online (macOS, Apple Silicon) - NHL Hockey '94 static recompilation
================================================================================

A personal macOS build of NHL Hockey '94 (Sega Genesis) statically
recompiled to native code on the segagenesisrecomp engine, with rollback
netplay over the internet (ICE). It plays against the matching Windows and macOS builds
of the same source: both report the same netplay version (see "Check" below).

NO GAME DATA IS INCLUDED. Bring your own ROM.


Credits
-------
This build's online features (per-player views, position lock, name tags) are
built on the reverse-engineered NHL 94 source by abdulahmad:
    https://github.com/abdulahmad/NHL94Genesis
with the work of chaos, StaticEck, McMarkis (Mhopkinsinc) and Drezz, and
special thanks to smozoma and kingraph.

Also built on NHL94Recomp by McMarkis (Mhopkinsinc)
(https://github.com/Mhopkinsinc/NHL94Recomp), the segagenesisrecomp engine by
Matthew Stanley (https://github.com/mstan/segagenesisrecomp), and recomp-net,
recomp-ui and recomp-net-server by RetroPortingToolKit
(https://github.com/RetroPortingToolKit). Thanks to the original developers of
NHL Hockey '94.

Source: this is a private, unpublished fork of the projects above (local
branches feat/netplay, nhl94-rollback, nhl94-no-signin). Ask the person who
gave you this build for the source.


1. Your ROM
-----------
You need your own dump of the USA/EU cartridge:

    NHL Hockey '94   1,048,576 bytes   CRC32 9438F5DD   serial GM T-50656 -00

Any file name works (e.g. NHL94.bin) and it can live anywhere; putting it in
this folder is simplest. Both players must use the SAME ROM image: in the
console window the line "genesis_netplay: lobby identity ... content=<sha256>"
shows its SHA-256, and the in-match handshake refuses a different one.


2. First start
--------------
Requirements: a Mac with Apple Silicon (M1 or newer) running macOS 27 or
later. Intel Macs and older macOS versions are not supported by this build.

Unzip everything into one folder and keep the files together (the .dylib
libraries and the assets/ folder must stay next to NHL94Recomp).

The app is not signed by Apple, so macOS blocks it when it came from another
computer. Once, in Terminal, clear the quarantine flag on the folder (drag
the folder onto the Terminal window to paste its path):

    xattr -dr com.apple.quarantine /path/to/NHL94Online-mac

  * NHL94.command     double-click to start (opens the launcher; a Terminal
                      window shows the log). Or from Terminal: ./NHL94Recomp

Control mode (launcher: Mods -> Gameplay -> Control mode):
  * Classic         the cartridge: your control switches to the player
                    nearest the puck (B / passes / loose pucks).
  * Position lock   you control ONE position all game, the game's AI plays
                    everyone else. Seat 1 = you offline / the room HOST
                    online (default Home Center), Seat 2 = the guest (default
                    Away Center); Seats 3-4 for 4-player rooms. One person per
                    position. Offline only the seats that have a controller
                    are human.
  * Format          6v6 (the cartridge) or 4v4: goalie, centre, left wing
                    and left D per team. A penalty gives the OTHER team an
                    extra (AI) skater, as in NHL 3-on-3 overtime. In 4v4 you
                    can pick G / LD / LW / C; preset "Full 4v4" fills 8 seats.
                    Online the host's format is used.
  Saved in nhl94-control.ini next to the program. Online the HOST's mode
  and positions are used by both players (the guest's own choice is kept
  for later); nothing has to match.

Pick your ROM in the launcher the first time; it is remembered (rom.cfg).
A Terminal window shows the log; keep it open, it helps when something fails.

3. Online match (lobby server)
------------------------------
The lobby server is preset to @NHL94_LOBBY_URL@
(Launcher -> Netplay -> network/lobby settings shows it and lets you change
it; the environment variable RNET_LOBBY_URL also overrides it).
No account or sign-in is needed: this server offers no Discord sign-in, and
this build then lets everyone play online as a guest (the console prints
"rnet_account: lobby server ... offers no sign-in; playing online as a guest").
Set your player name in the launcher so the other side sees who you are.

  1. Launcher -> Netplay -> "Online Netplay".
  2. HOST: press "Host Lobby", give the room a name, Max Players 2,
     "Create Lobby". Wait for the other player to appear in the room.
     GUEST: find the room in the list and press "Join" (guests are set
     ready automatically).
  3. The host presses PLAY. The game boots on both sides ("Starting match").
  4. On the menus choose Players = "Two - Head to Head". Either player's pad
     can drive the menus. (Before hosting, the host picks Classic or Position
     lock in Mods -> Control mode; the guest gets the host's choice.)
     Position lock: each player seat in the room shows its position
     ("Home C", "Away G"). Press Take on an open seat (or drag yourself
     there) to play that position; empty seats are played by the CPU. For a
     free choice of all 12 positions the host uses the Full 6v6 list and a
     12-player room.
  After the match you return to the room and can play again.

The match runs peer-to-peer over ICE (UDP; STUN stun.l.google.com:19302;
the lobby offers no TURN relay). If macOS asks whether NHL94Recomp may accept incoming network
connections, choose Allow. If the two networks
cannot reach each other directly (strict/symmetric NAT on both sides), the
match cannot connect.

Fallback: Netplay -> LAN / Direct IP. The host must then forward its game
UDP port (shown in the host panel, default 7777) on its router, and the guest
enters the host's public IP.


4. Controls (defaults; rebind in the launcher or settings.ini)
---------------------------------------------------------------
  Keyboard: arrows = move, Z = A, X = B, C = C, Return = Start
  Gamepad (Xbox layout): south (A) = B, east (B) = A, west (X) = C, Start

  NHL 94: C = shoot / (defense) hook-check; B = pass / (defense) body check;
  A = speed burst / (classic mode) switch player.

  Position lock mode:
    X key / gamepad south button (= Genesis B) while an AI TEAMMATE carries
      the puck: call for a pass to you (he passes when he can).
    G key / left-stick click (L3): camera toggle - follow your player <->
      follow the puck (only on your screen, never sent to the other side).
    F key / right-stick click (R3): view zoom 1x / 1.25x / 1.5x / 2x (default
      1.5x in Position lock; also Mods -> Control mode -> View zoom). Only on
      your screen.
    Your team always attacks up the screen; your player is marked.

  F11 fullscreen (or the green window button), F10 runtime menu, Esc quit. Do not use turbo (Tab) online.


5. Check / troubleshooting
--------------------------
The console prints, at start:
    genesis_netplay: lobby identity game="NHL Hockey '94" version=dev+<build hash> ...
Both players must show the SAME version (dev+<hash>; Windows and Mac zips
made from the same commit match). In a
match, "peer identity matches" means both sides run the same code, ROM and
settings; "MOD SETS DIFFER" / "BOOT DIGEST MISMATCH" mean a different ROM or build.
The line "nhl94_seats: control mode ... (source: ...)" says which mode runs
and where it came from (launcher setting / host's session config / the
NHL94_SEATS environment variable, which overrides the launcher). Logs and feedback replays are written under logs/.


License
-------
The recompiled runtime and this package: PolyForm Noncommercial License 1.0.0
(LICENSE). Third-party components and their licenses: THIRD-PARTY-LICENSES.md
and the licenses/ folder (sdl2-compat + SDL3 zlib, libjuice MPL-2.0, recomp-ui /
Dear ImGui / recomp-net / rbengine MIT, fonts OFL-1.1 / CC BY-SA 4.0, and the
bundled FreeType, HarfBuzz, GLib, gettext libintl, PCRE2, libpng, Brotli and
Graphite2 libraries under their own licenses).
The game itself remains the property of its rights holders; this package
contains none of it.

Source: NHL94Recomp (github.com/Mhopkinsinc/NHL94Recomp base) with the
segagenesisrecomp engine (github.com/mstan/segagenesisrecomp), recomp-ui and
recomp-net (github.com/RetroPortingToolKit). libjuice source:
https://github.com/paullouisageneau/libjuice (v1.7.2, unmodified).
