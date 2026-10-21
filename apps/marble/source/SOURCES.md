# Corresponding source: Marble Orbit

Marble Orbit is our own program, written for the badge. This directory is
its source (it is no longer part of why2025-linux, where it started in
`sdapps/marble` at commit `ebdcc16`); the drawing code and fonts are copied
from why2025-linux `patches/buildroot/overlay-src/` at commit `c629a1c`:

| File | What |
|---|---|
| `marble.c` | the game: physics, the elements, drawing, the keys, the scores |
| `levels.h` | the 18 levels as text, with the tile legend |
| `test_marble.py` | its host test: the levels checked, the game played by script, screens saved as PPM |
| `app.conf` | the menu entry of a hand-installed copy (the store writes its own) |
| `wbdraw.h`, `home_assets.h`, `home-assets/wb_tokens.h` | the menu's drawing code, fonts and icons, and the design tokens (also in `apps/flappy/source`) |

Build, with the badge's toolchain ([toolchain/](../../../toolchain/README.md)),
in this directory:

```
T=$BR_OUT/host/bin/riscv32-buildroot-linux-uclibc
$T-gcc -Os -fPIC -static -ffunction-sections -fdata-sections -Wall -I. marble.c \
    -Wl,--gc-sections -Wl,--no-relax "-Wl,-elf2flt=-r -s 16384" -o marble
```

On a Linux or macOS host, `python3 test_marble.py` builds it with
`-DMARBLE_HOST` and runs 242 checks: every level's format and a route from
start to ring, the physics of each element, lives and scores, the menus,
and an autopilot that finishes every level within its par time.

Measured on the badge (2026-10-20, from flash, started from Games): about
15 ms to draw and write a frame, as only the rows that change are written;
2.2 ms to read the accelerometer (x and y). The app log
(`/tmp/app-marble.log`) gets these numbers on exit. The tilt direction and
feel were checked by hand on 2026-10-21.
Test hook: key codes written to `/tmp/marble.keys` count as key presses (a
code plus 1000 is that key let go).
