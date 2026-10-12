# Corresponding source: MicroPython

Together with the download below, this directory is the complete
corresponding source of the `micropython` package (docs/package.md §2.10).
The files here are the badge-specific ones, copied from why2025-linux
`sdapps/micropython/` at commit `fa51576`; the toolchain and the build
script are in [toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 |
|---|---|---|
| MicroPython 1.29.0 (with its submodules: mbedTLS, micropython-lib, uzlib, re1.5) | https://github.com/micropython/micropython/releases/download/v1.29.0/micropython-1.29.0.tar.xz | `d925a7c664e79a2bdf3dfcb285ba5e2237041cc35a0bd4ee573b6c5711efeca0` |

MicroPython is unchanged: the Unix port, built with make options only
(no threads, ffi, btree or VFS of its own; see `build_micropython` in
the build script).

| File | What |
|---|---|
| `start` | Sets the heap and the module path (APP_DATA/lib) and starts the REPL, a script, or a script from Files |
| `app.conf` | The menu entry of the hand-installed app (the store writes its own) |

Build: put these files in `sdapps/micropython/` next to
`toolchain/sdapps-build.sh` and run `sdapps-build.sh micropython`
(toolchain/README.md). The build also compiles `mpy-cross` for the build
machine, to freeze the bundled Python modules.
