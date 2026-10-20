# Corresponding source: Flappy

Flappy is our own program, written for the badge. This directory is its
source (it is not part of why2025-linux); the drawing code and fonts are
copied from why2025-linux `patches/buildroot/overlay-src/` at commit
`c629a1c`:

| File | What |
|---|---|
| `flappy.c` | the game: the world, the screen, the keys, the best score |
| `test_flappy.py` | its host test: the game played by script, screens saved as PPM |
| `app.conf` | the menu entry of a hand-installed copy (the store writes its own) |
| `wbdraw.h`, `home_assets.h`, `home-assets/wb_tokens.h` | the menu's drawing code, fonts and icons, and the design tokens (also in `apps/calc/source`) |

Build, with the badge's toolchain ([toolchain/](../../../toolchain/README.md)),
in this directory:

```
T=$BR_OUT/host/bin/riscv32-buildroot-linux-uclibc
$T-gcc -Os -fPIC -static -ffunction-sections -fdata-sections -Wall -I. flappy.c \
    -Wl,--gc-sections -Wl,--no-relax "-Wl,-elf2flt=-r -s 16384" -o flappy
```

On a Linux or macOS host, `python3 test_flappy.py` builds it with
`-DFLAPPY_HOST` and plays it (19 checks).

Measured on the badge (2026-10-20): about 41 ms a frame (24 frames a
second); writing the panel takes most of it, as the kernel turns every
written row into the scan-out buffer. The app log
(`/tmp/app-flappy.log`) gets the frame count and time per frame on exit.
Test hook: key codes written to `/tmp/flappy.keys` count as key presses.
