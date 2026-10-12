# Corresponding source: Links

Together with the downloads below, this directory is the complete
corresponding source of the `links` package (docs/package.md §2.10). The
files here are the badge-specific ones, copied from why2025-linux
`sdapps/` at commit `2be6d7c`; the toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| Links | http://links.twibright.com/download/links-2.30.tar.bz2 | `c4631c6b5a11527cdc3cb7872fc23b7f2b25c2b021d596be410dadb40315f166` |
| OpenSSL | https://github.com/openssl/openssl/releases/download/openssl-3.5.6/openssl-3.5.6.tar.gz | `deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736` |
| zlib | from the Buildroot toolchain (toolchain/README.md) | `Buildroot 2025.02.15` |
| CA certificates | https://curl.se/ca/cacert.pem as of 2026-09-25 | `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505` |

Build: put these files in `sdapps/links/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh links` (toolchain/README.md).

The store's `start` (in the app's `overlay/`) replaces the one built by
the script: it also sets HOME and passes all arguments on.
