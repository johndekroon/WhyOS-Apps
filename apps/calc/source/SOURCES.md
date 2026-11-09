# Corresponding source: Calculator

The calculator is our own program, written for the badge. The files are
copied from WhyOS `sdapps/calc/` and `libwb/` at commit `3ef5aae`:

| File | What |
|---|---|
| `calc.c` | the calculator: the sums, the screen, the keys |
| `test_calc.py` | its host test: sums typed, results checked |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `libwb/` | WhyOS's drawing library `libwb/` (`wb.h` and its parts: the design tokens, fonts and icons, the frontpanel's scene, glass, pop-ups, drawing only what changed; `keyrep.h`, which held keys repeat), as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: WhyOS `libwb/gen/fonts/`) |

Build: `sdapps-build.sh calc` ([toolchain/](../../../toolchain/README.md)),
with `calc.c` and `app.conf` in `sdapps/calc/` and `libwb/` next to
`sdapps/`, as in WhyOS. On a Linux or macOS host, `python3
test_calc.py` (in that layout) builds and tests it.
