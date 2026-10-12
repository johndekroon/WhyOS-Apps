# Corresponding source: DOOM

Together with the downloads below, this directory is the complete
corresponding source of the `doom` package (docs/package.md §2.10). The
files here are copied from why2025-linux at commit `40e60f6`; the
toolchain and the build script are in [toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| fbDOOM | https://github.com/maximevince/fbDOOM/archive/17280163bc95e5d954d2efaa0633489b763b4cd1.tar.gz | `77f57cee68fed438dffdba96f6070b8975c16652a63ddf4fb967994e5585a38a` |
| DOOM shareware 1.9 (the WAD, data only) | http://www.jbserver.com/downloads/games/doom/misc/shareware/doom19s.zip | `cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6` |

| File | What |
|---|---|
| `0001-i_system-I_Quit-and-I_Error-must-exit.patch` | our change to fbDOOM (`patches/buildroot/global-patches/fbdoom/`): it went on drawing into freed memory after quitting |
| `doom.c` | starts fbDOOM with a zone that fits, and ends it with its own quit key (`sdapps/doom/`) |

Build: put `doom.c` in `sdapps/doom/` and the patch in
`patches/buildroot/global-patches/fbdoom/` and run `sdapps-build.sh doom`.
