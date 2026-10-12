# Corresponding source: NetSurf

Together with the downloads below, this directory is the complete
corresponding source of the `netsurf` package (docs/package.md §2.10). The
files here are the badge-specific ones, copied from why2025-linux
`sdapps/` at commit `2be6d7c`; the toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| NetSurf (netsurf-all) | https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz | `4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b` |
| Mbed TLS | https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.6/mbedtls-3.6.6.tar.bz2 | `8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a` |
| curl | https://curl.se/download/curl-8.20.0.tar.xz | `63fe2dc148ba0ceae89922ef838f7e5c946272c2e78b7c59fab4b79d3ce2b896` |
| libpng | http://downloads.sourceforge.net/project/libpng/libpng16/1.6.58/libpng-1.6.58.tar.xz | `28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775` |
| Expat | https://github.com/libexpat/libexpat/releases/download/R_2_8_1/expat-2.8.1.tar.xz | `10b195ee78160a908388180a8fe3603d4e9a12f4755fbf5f3816b23a9d750da0` |
| libjpeg | http://www.ijg.org/files/jpegsrc.v9f.tar.gz | `04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b` |
| zlib | from the Buildroot toolchain (toolchain/README.md) | `Buildroot 2025.02.15` |
| CA certificates | https://curl.se/ca/cacert.pem as of 2026-09-25 | `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505` |

Build: put these files in `sdapps/netsurf/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh netsurf` (toolchain/README.md).

The sha256 of netsurf-all is the tarball that was used; the build
script does not check it yet. `start` is in the app's `overlay/`.
