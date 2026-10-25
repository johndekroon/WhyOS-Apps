# Corresponding source: PDF viewer (pdfview)

Together with the download below, this directory is the complete
corresponding source of the `pdfview` package (docs/package.md §2.10).
The files here are the badge-specific ones, copied from why2025-linux
`sdapps/` at commit `932f5ee`; the toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 |
|---|---|---|
| MuPDF 1.28.5 (with FreeType, jbig2dec, libjpeg, zlib and the URW fonts in its `thirdparty/` and `resources/`) | https://mupdf.com/downloads/archive/mupdf-1.28.5-source.tar.gz | `98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934` |

Build: put these files in `sdapps/pdfview/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh pdfview` (toolchain/README.md). MuPDF is built
unchanged, with the options in the script's `MUPDF_OPTS` and
`MUPDF_DEFS`; it compiles two C++ stubs, so the script needs a C++
compiler for the badge (`CXX_OUT`).

`pdfview.c` is the program (AGPL-3.0-or-later, as MuPDF); `wbdraw.h`
(drawing), `keyrep.h` (held keys) and `home_assets.h` (fonts rendered
to bitmaps) come from the badge menu, `patches/buildroot/overlay-src/`
in why2025-linux. `test/` is its host test (`make -C test`, in a Linux
build machine with MuPDF's source unpacked).
