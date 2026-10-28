# Corresponding source: Marble Orbit

Marble Orbit is our own program, written for the badge. This directory is
its source (it is no longer part of why2025-linux, where it started in
`sdapps/marble` at commit `ebdcc16`); the drawing code, fonts and design
tokens are a copy of why2025-linux's drawing library `libwb/` (commit
`647c8ea`):

| File | What |
|---|---|
| `marble.c` | the game: physics, the elements, drawing, the keys, the scores |
| `levels.h` | the 18 levels as text, with the tile legend |
| `test_marble.py` | its host test: the levels checked, the game played by script, screens saved as PPM |
| `app.conf` | the menu entry of a hand-installed copy (the store writes its own) |
| `libwb/` | the drawing library (`wb.h` and its parts: tokens, fonts and icons, glass, the frontpanel's figures behind the title; its `README.md`), as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: why2025-linux `libwb/gen/fonts/`) |

Build, with the badge's toolchain ([toolchain/](../../../toolchain/README.md)),
in this directory (in a checkout of the repos `-I` may point at
why2025-linux's `libwb/` instead):

```
T=$BR_OUT/host/bin/riscv32-buildroot-linux-uclibc
$T-gcc -Os -fPIC -static -ffunction-sections -fdata-sections -Wall -Ilibwb -I. marble.c \
    -Wl,--gc-sections -Wl,--no-relax "-Wl,-elf2flt=-r -s 16384" -o marble
```

On a Linux or macOS host, `python3 test_marble.py` builds it with
`-DMARBLE_HOST` and runs 251 checks: every level's format and a route from
start to ring, the physics of each element, lives and scores, the menus,
and an autopilot that finishes every level within its par time. With a
directory as argument it saves the screens there as PPM (title, ready,
won, over, pause, help, settings, sensitivity, results, a level a world).

Measured on the badge (2026-10-20, from flash, started from Games): about
15 ms to draw and write a frame, as only the rows that change are written;
2.2 ms to read the accelerometer (x and y). The app log
(`/tmp/app-marble.log`) gets these numbers on exit. The tilt direction and
feel were checked by hand on 2026-10-21.
Test hook: key codes written to `/tmp/marble.keys` count as key presses (a
code plus 1000 is that key let go).

The look (2026-10-27) is the canvas "Menu's met frontpaneel" as libwb
draws it: the title (the levels) lies on the frontpanel's figures (libwb's
scene) with its tiles as glass; a level fills the panel and stays as it
was, the cards over it and the hint bar are glass; the pause, the keys, the
settings and the results are frosted pop-ups with their keys inside;
square corners, the focus cyan. The title and a pop-up are recorded first
and only the bands that changed are drawn (libwb's `wb_bands` and
`wb_frame`): a move on the title draws two tiles' rows, a key in a pop-up
its changed rows with the blur kept. A pop-up is drawn in a whole picture
(a 1 MB buffer taken while it is open), as blurring needs it; without the
memory it is drawn in bands on a plain panel. `MARBLE_UI_TIMING=1` in the
environment writes a line per frame to the app log (its time, how it was
drawn, the bands, libwb's scene and glass time in µs). The host test also
draws key sequences (title, pause, a won level) twice, with and without
`WB_FULL=1`, and compares every frame.
