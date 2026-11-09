# Corresponding source: Game Boy (gnuboy)

Together with the downloads below, this directory is the complete
corresponding source of the `gnuboy` package (docs/package.md §2.10). The
files here are the badge-specific ones, copied from WhyOS
`sdapps/` and `libwb/` at commit `3ef5aae`; the toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| gnuboy | https://github.com/rofl0r/gnuboy | `c367bb4ba96fb07cd62f72f5ecb43aeff7012564` |
| tinyalsa 2.0.0 | https://github.com/tinyalsa/tinyalsa/archive/v2.0.0/tinyalsa-2.0.0.tar.gz | `573ae0b2d3480851c1d2a12503ead2beea27f92d44ed47b74b553ba947994ef1` |

Build: put these files in `sdapps/gnuboy/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh gnuboy` (toolchain/README.md).

`badge.c` is the badge backend (`sys/badge/`); `badge.rc` the key bindings.
`pause.c` and `pause.h` (WhyOS `sdapps/pause/`) draw the pause
screen that Esc opens, with WhyOS's drawing library
`libwb/` (as built); put `pause.*` in `sdapps/pause/` and `libwb/` next to
`sdapps/`, as in WhyOS.

The sound goes through bsound (`sound/bsound.c`, `sound/bsound.h`; WhyOS
`sdapps/sound/`), a thread that plays on the output chosen in Settings >
Sound, built on tinyalsa with `tinyalsa/0001-sync-ptr-without-mmap.patch`
and `tinyalsa/noplugins.c` (WhyOS `sdapps/tinyalsa/`); put `sound/` and
`tinyalsa/` in `sdapps/`, as in WhyOS.
