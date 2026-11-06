# Corresponding source: Flashlight

The flashlight is our own program, written for the badge. The files are
copied from WhyOS `sdapps/flashlight/`, `libwb/` and
`patches/buildroot/overlay-src/`:

| File | What |
|---|---|
| `flashlight.c` | the flashlight: the modes, the screen, the keys |
| `test_flashlight.py` | its host test: keys pressed, modes and screens checked |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `libwb/` | WhyOS's drawing library `libwb/`, as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: WhyOS `libwb/gen/fonts/`) |
| `power.h` | the power management's key time, which the app keeps fresh |

Build: `sdapps-build.sh flashlight` ([toolchain/](../../../toolchain/README.md)),
with `flashlight.c` and `app.conf` in `sdapps/flashlight/`, `libwb/` next
to `sdapps/` and `power.h` in `patches/buildroot/overlay-src/`, as in
WhyOS. On a Linux or macOS host, `python3 test_flashlight.py` (in that
layout) builds and tests it.
