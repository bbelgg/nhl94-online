# Tracking upstream

This project never sends changes upstream. Every repository we modify carries
our changes as a **patch series on top of upstream**: a local branch that is
upstream plus our commits. Syncing means replaying those commits onto the
newest upstream, building, testing, and only then switching over.

| Repository | Our branch | Follows |
|---|---|---|
| `NHL94Recomp` (this repo) | `feat/netplay` | `origin/master` (Mhopkinsinc/NHL94Recomp) |
| `segagenesisrecomp` | `nhl94-rollback` | `upstream/master` (mstan/segagenesisrecomp) |
| `segagenesisrecomp/external/recomp-net` | `nhl94-no-signin` | `origin/main` (its own tip, see `follow-tip.txt`) |
| `segagenesisrecomp/external/m68k-recomp-core` | `nhl94-rollback` | the commit the upstream engine pins |
| `segagenesisrecomp/external/rbengine`, `z80-recomp-core` | none (no changes) | the commit the upstream engine pins |
| `recomp-ui` | `nhl94-no-signin` | `origin/master` (RetroPortingToolKit/recomp-ui) |

The engine's libraries follow the **engine's pin** by default (`FOLLOW=pin`), except those listed in `tools/upstream-sync/follow-tip.txt` (recomp-net), which follow their own upstream tip:
upstream tested that combination together. `FOLLOW=tip` takes each library's
own newest commit instead, which is more likely to need work in our patches.

## Routine sync

```sh
tools/upstream-sync/sync.sh status     # how far behind each repo is
tools/upstream-sync/sync.sh trial      # rebase our patches onto upstream, on branch sync/trial
tools/upstream-sync/sync.sh gate       # build + test the trial (see below)
tools/upstream-sync/sync.sh promote    # only after GATE: PASS
```

`trial` first tags every current head `pre-sync/<stamp>`, so nothing is lost.
Submodule pointer conflicts are resolved automatically. A real conflict stops
with the repository and files named: resolve it, `git rebase --continue` in that
repository, and rerun `trial` (finished repositories are skipped).
`sync.sh abort` puts everything back on the old branches.

After `promote`, rebuild and repackage the Mac and Windows zips. Every player
needs the new build: builds from different syncs will not play each other.

## The gate (`tools/upstream-sync/gate.sh`)

Builds into `~/nhl94-online-build/sync/` and must end in
`GATE: PASS`:

1. Builds: native Mac (also the code generator), the portable universal Mac
   build and the Windows cross build, all release (`GEN_ENABLE_TRACE=OFF`).
2. Mac and Windows report the same netplay source identity.
3. Struct layout probe (`layout_probe.c`): the machine state has the same size
   and offsets under clang arm64, clang x86_64 and mingw-w64. A difference here
   is a guaranteed Mac-vs-Windows `boot_digest_mismatch`.
4. Unit tests of recomp-net, the 68000 core and recomp-ui. Tests that also fail on the unmodified upstream commit are listed, with the reason, in `known-failures.txt` and skipped; failing tests get 5 attempts (some upstream network tests are timing-flaky).
5. `tools/seats/run_checks.sh` and `run_checks12.sh`: rollback determinism
   probes (`rb_probe`) for 1-12 seats, the call-for-pass and lock scripts, and
   a 2-process localhost rollback match with identical TIMELINE digests.
6. No dispatch misses.
7. Online rooms through the real lobby (`tools/net/room.sh`): 2 players, and 12
   players plus a spectator, and a 12-seat 6v6 room where the guest takes
   seat 11 (Away G) with CPU seats between the humans; matching boot and
   TIMELINE digests.
   `gate.sh --quick` skips this step.

State hashes are compared with `tools/upstream-sync/baseline/`. A difference is
only a warning: an upstream emulation fix may legitimately change the game's
state. Check what changed, then copy the new `*.hashes` files from the sync
build root into `baseline/`.

## Keeping patches easy to replay

- Put our logic in our own files (`game/`, `runner/multiview.c`, ...); keep
  edits inside upstream files to small hooks.
- Make behaviour changes that affect every game opt-in switches, not edits of
  upstream's default path.
- One concern per commit, so a conflicting commit is easy to understand.
- Sync often. Small syncs are easy; big ones are not.

## Backup

These branches exist only on this machine until they are pushed to a private
remote of your own. Never push them to the upstream projects.
