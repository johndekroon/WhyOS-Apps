# Corresponding source: Game Boy (gnuboy)

Together with the downloads below, this directory is the complete
corresponding source of the `gnuboy` package (docs/package.md §2.10). The
files here are the badge-specific ones, copied from why2025-linux
`sdapps/` at commit `be892b8`; the toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| gnuboy | https://github.com/rofl0r/gnuboy | `c367bb4ba96fb07cd62f72f5ecb43aeff7012564` |

Build: put these files in `sdapps/gnuboy/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh gnuboy` (toolchain/README.md).

`badge.c` is the badge backend (`sys/badge/`); `badge.rc` the key bindings.
`pause.c` and `pause.h` (why2025-linux `sdapps/pause/`) draw the pause
screen that Esc opens, with the menu's `wbdraw.h`, `home_assets.h` and
`home-assets/wb_tokens.h` (why2025-linux `patches/buildroot/overlay-src/`);
put `pause.*` in `sdapps/pause/` and the others in `patches/buildroot/overlay-src/`.
