# Corresponding source: NetHack

Together with the downloads below, this directory is the complete
corresponding source of the `nethack` package (docs/package.md §2.10).
The files here are the badge-specific ones, copied from why2025-linux
`sdapps/nethack/` at commit `60835d6`; the toolchain and the build script
are in [toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 or commit |
|---|---|---|
| NetHack 3.6.7 | https://www.nethack.org/download/3.6.7/nethack-367-src.tgz | `98cf67df6debf9668a61745aa84c09bcab362e5d33f5b944ec5155d44d2aacb2` |
| zlib | from the Buildroot toolchain (toolchain/README.md) | `Buildroot 2025.02.15` |

| File | What |
|---|---|
| `0001-badge.patch` | Changes to NetHack's source (termcap without ncurses, colours, paths, locking, SIGTERM) |
| `badge.c` | The arrow keys as moves; a `fork()` that fails |
| `start` | Sets up APP_DATA and APP_CONFIG and starts the game |
| `termcap`, `sysconf`, `nethackrc` | The terminal entry, the system settings, the default options |
| `app.conf` | The menu entry of the hand-installed app (the store writes its own) |

Build: put these files in `sdapps/nethack/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh nethack` (toolchain/README.md). The build runs
NetHack's own tools (makedefs, lev_comp, dgn_comp, dlb) built for the
badge, through qemu-user (`apt install qemu-user`), so the level files
have the badge's type sizes.
