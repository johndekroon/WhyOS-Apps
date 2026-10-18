# Corresponding source: Picture viewer (imgview)

Together with the downloads below, this directory is the complete
corresponding source of the `imgview` package (docs/package.md §2.10). The
files here are the badge-specific ones, copied from why2025-linux
`sdapps/` at commit `be892b8`; the toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| libjpeg | http://www.ijg.org/files/jpegsrc.v9f.tar.gz | `04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b` |
| libpng | http://downloads.sourceforge.net/project/libpng/libpng16/1.6.58/libpng-1.6.58.tar.xz | `28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775` |
| zlib | from the Buildroot toolchain (toolchain/README.md) | `Buildroot 2025.02.15` |

Build: put these files in `sdapps/imgview/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh imgview` (toolchain/README.md).

`imgview.c` is the program; `wbdraw.h` (drawing) and `home_assets.h` (fonts
rendered to bitmaps by why2025-linux `overlay-src/home-assets/gen.py`) come from the
badge menu, `patches/buildroot/overlay-src/` in why2025-linux; the build
script expects them there.
