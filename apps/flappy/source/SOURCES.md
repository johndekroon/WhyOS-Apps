# Corresponding source: Flappy

Flappy is our own program, written for the badge. This directory is its
source (it is not part of why2025-linux); the drawing code, fonts and
design tokens are a copy of why2025-linux's drawing library `libwb/`
(commit `c8cfa09`):

| File | What |
|---|---|
| `flappy.c` | the game: the world, the screen, the keys, the best score |
| `test_flappy.py` | its host test: the game played by script, screens saved as PPM |
| `app.conf` | the menu entry of a hand-installed copy (the store writes its own) |
| `libwb/` | the drawing library (`wb.h` and its parts: tokens, fonts and icons, glass; its `README.md`), as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: why2025-linux `libwb/gen/fonts/`) |

Build, with the badge's toolchain ([toolchain/](../../../toolchain/README.md)),
in this directory (in a checkout of the repos `-I` may point at
why2025-linux's `libwb/` instead):

```
T=$BR_OUT/host/bin/riscv32-buildroot-linux-uclibc
$T-gcc -Os -fPIC -static -ffunction-sections -fdata-sections -Wall -Ilibwb -I. flappy.c \
    -Wl,--gc-sections -Wl,--no-relax "-Wl,-elf2flt=-r -s 16384" -o flappy
```

On a Linux or macOS host, `python3 test_flappy.py` builds it with
`-DFLAPPY_HOST` and plays it (25 checks); with a directory as argument
it saves the screens there as PPM.

Measured on the badge (2026-10-20): about 41 ms a frame (24 frames a
second); writing the panel takes most of it, as the kernel turns every
written row into the scan-out buffer. The app log
(`/tmp/app-flappy.log`) gets the frame count and time per frame on exit.
Test hook: key codes written to `/tmp/flappy.keys` count as key presses.

The look (2026-10-27) is the canvas "Menu's met frontpaneel" as libwb
draws it: the game field fills the panel and stays as it was; the cards
over it and the hint bar are glass, the pause and the keys are frosted
pop-ups with their keys inside, square corners, the focus cyan. While the
game moves every band is drawn; standing still (a pop-up, the game over)
a frame is recorded first and only the bands that changed are drawn
(libwb's `wb_bands` and `wb_frame`), so a key in the pause draws its
changed rows with the blur kept. A pop-up is drawn in a whole picture (a
1 MB buffer taken while it is open), as blurring needs it; without the
memory it is drawn in bands on a plain panel. `FLAPPY_UI_TIMING=1` in the
environment writes a line per frame to the app log (its time, how it was
drawn, the bands, libwb's glass time in µs). The host test also draws key
sequences (the pause, the keys, the game over) twice, with and without
`WB_FULL=1`, and compares every frame.
