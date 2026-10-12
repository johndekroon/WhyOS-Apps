# Corresponding source: nano

Together with the downloads below, this directory is the complete
corresponding source of the `nano` package (docs/package.md §2.10).
The files here are the badge-specific ones, copied from why2025-linux
`sdapps/nano/` at commit `2dc2819`; the toolchain and the build script
are in [toolchain/](../../../toolchain/README.md).

| Upstream | URL | SHA-256 |
|---|---|---|
| GNU nano 9.2 | https://www.nano-editor.org/dist/v9/nano-9.2.tar.xz | `05ecb99247b782e8a5b3a25ed4101dd034b0236902f7449bc9795b717642f7e9` |
| ncurses 6.6 | https://ftp.gnu.org/gnu/ncurses/ncurses-6.6.tar.gz | `355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11` |

nano and ncurses are unchanged; ncurses is built static, without terminfo
files, with the terminals it knows built in (`--with-fallbacks`).

| File | What |
|---|---|
| `appterm.ti` | The terminfo entry of the menu's terminal window, added to ncurses' `misc/terminfo.src` before the build |
| `nofork.c` | A `fork()` that fails (there is none on the badge) |
| `start` | Picks the terminal entry, puts the history on the card, puts the settings together and starts nano |
| `nanorc` | The default settings |
| `app.conf` | The menu entry of the hand-installed app (the store writes its own) |

Build: put these files in `sdapps/nano/` next to `toolchain/sdapps-build.sh`
and run `sdapps-build.sh nano` (toolchain/README.md). The build also
compiles ncurses for the build machine, for its `tic` and `infocmp`.
