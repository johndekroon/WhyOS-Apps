# Corresponding source: nmap

Together with the downloads below, this directory is the complete
corresponding source of the `nmap` package (docs/package.md §2.10). The
files here are the badge-specific ones, copied from why2025-linux
`sdapps/` at commit `2be6d7c`; the toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| nmap | https://nmap.org/dist/nmap-7.95.tar.bz2 | `e14ab530e47b5afd88f1c8a2bac7f89cd8fe6b478e22d255c5b9bddb7a1c5778` |

Build: put these files in `sdapps/nmap/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh nmap` (toolchain/README.md).

nmap is C++: the toolchain needs C++ (why2025-linux `Tools/usb/br.sh cxx`, or
`BR2_TOOLCHAIN_BUILDROOT_CXX=y` on top of the defconfig). Buildroot's
`package/nmap/0001-libdnet-always-build-a-static-library.patch` is applied.
