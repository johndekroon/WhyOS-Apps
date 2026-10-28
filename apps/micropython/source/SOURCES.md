# Corresponding source: MicroPython

Together with the download below, this directory is the complete
corresponding source of the `micropython` package (docs/package.md §2.10).
The files here are the badge-specific ones, copied from why2025-linux
`sdapps/micropython/` at commit `8a412f9` (2026-10-28; the tests there are
left out); the toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 |
|---|---|---|
| MicroPython 1.29.0 (with its submodules: mbedTLS, micropython-lib, uzlib, re1.5) | https://github.com/micropython/micropython/releases/download/v1.29.0/micropython-1.29.0.tar.xz | `d925a7c664e79a2bdf3dfcb285ba5e2237041cc35a0bd4ee573b6c5711efeca0` |

meshcore_py and pycayennelpp are vendored here unmodified, in
`meshcore-py/upstream/`, from the commits in `meshcore-py/VENDORED`
(https://github.com/meshcore-dev/meshcore_py,
https://github.com/smlng/pycayennelpp).

| File | What |
|---|---|
| `start` | Sets the heap and the module path (APP_DATA/lib) and starts the REPL, a script, or a script from Files |
| `app.conf` | The menu entry of the hand-installed app (the store writes its own) |
| `0001-int-from_bytes-signed.patch` | MicroPython: `int.from_bytes()` takes `signed=`, which meshcore_py uses |
| `0002-getaddrinfo-addrconfig.patch` | MicroPython: `socket.getaddrinfo()` returns only the address families the badge has (no IPv6) |
| `meshcore-py/` | meshcore_py 2.3.17 and pycayennelpp as frozen in: `upstream/`, our `patches/` and `port/` (the MicroPython glue), `stage.sh` that puts them together, `examples/ping_bot.py` |
| `modules/` | Our own modules, frozen in: `setting.py` (the badge's settings), `ics.py` (calendars), `xml/` (`xml.etree.ElementTree`) |

Build: put these files in `sdapps/micropython/` next to
`toolchain/sdapps-build.sh` and run `sdapps-build.sh micropython`
(toolchain/README.md). The build applies the patches, stages meshcore_py,
and compiles `mpy-cross` for the build machine to freeze the bundled
Python modules.
