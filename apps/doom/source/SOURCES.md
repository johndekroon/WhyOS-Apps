# Corresponding source: DOOM

Together with the downloads below, this directory is the complete
corresponding source of the `doom` package (docs/package.md §2.10). The
files here are copied from WhyOS at commit `3ef5aae`; the
toolchain and the build script are in [toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| fbDOOM | https://github.com/maximevince/fbDOOM/archive/17280163bc95e5d954d2efaa0633489b763b4cd1.tar.gz | `77f57cee68fed438dffdba96f6070b8975c16652a63ddf4fb967994e5585a38a` |
| DOOM shareware 1.9 (the WAD, data only) | http://www.jbserver.com/downloads/games/doom/misc/shareware/doom19s.zip | `cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6` |
| tinyalsa 2.0.0 | https://github.com/tinyalsa/tinyalsa/archive/v2.0.0/tinyalsa-2.0.0.tar.gz | `573ae0b2d3480851c1d2a12503ead2beea27f92d44ed47b74b553ba947994ef1` |

| File | What |
|---|---|
| `0001-i_system-I_Quit-and-I_Error-must-exit.patch` | our change to fbDOOM (`patches/buildroot/global-patches/fbdoom/`): it went on drawing into freed memory after quitting |
| `0002-i_video_fbdev-whole-width.patch` | the panel's whole width at 4:3, and less work per pixel |
| `0003-i_video_fbdev-ppa.patch` | the picture scaled through `/dev/ppa` where the badge has it |
| `0004-saves-in-the-current-directory-backspace-deletes.patch` | saved games in the current directory, and Backspace only deletes |
| `0001-badge-sound.patch`, `i_badgesound.c` | the sound effects (`sdapps/doom/`): a sound module for fbDOOM's `i_sound.c` that mixes up to 16 effects for bsound; applied after the four patches above |
| `doom.c` | starts fbDOOM with a zone that fits, in the app's data folder, and ends it (`sdapps/doom/`) |
| `sound/bsound.c`, `sound/bsound.h` | bsound (`sdapps/sound/`): a thread that plays a game's sound on the output chosen in Settings > Sound |
| `tinyalsa/0001-sync-ptr-without-mmap.patch`, `tinyalsa/noplugins.c` | tinyalsa as bsound uses it, without mmap and plugins (`sdapps/tinyalsa/`) |

Build: put `doom.c`, `0001-badge-sound.patch` and `i_badgesound.c` in
`sdapps/doom/`, `sound/` and `tinyalsa/` in `sdapps/`, and the four
numbered fbDOOM patches in `patches/buildroot/global-patches/fbdoom/`, and
run `sdapps-build.sh doom`.
