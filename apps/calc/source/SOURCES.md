# Corresponding source: Calculator

The calculator is our own program, written for the badge. The files are
copied from why2025-linux `sdapps/calc/` and `patches/buildroot/overlay-src/`
at commit `be892b8`:

| File | What |
|---|---|
| `calc.c` | the calculator: the sums, the screen, the keys |
| `test_calc.py` | its host test: sums typed, results checked |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `wbdraw.h`, `home_assets.h`, `home-assets/wb_tokens.h` | the menu's drawing code, fonts and icons, and the design tokens (also in `apps/buspirate/source`) |
| `wbstatus.h`, `lora_state.h` | the menu's status bar and the LoRa state it shows |

Build: `sdapps-build.sh calc` ([toolchain/](../../../toolchain/README.md)),
with `calc.c` and `app.conf` in `sdapps/calc/` and the other files in
`patches/buildroot/overlay-src/` of why2025-linux. On a Linux or macOS
host, `python3 test_calc.py` (next to those files) builds and tests it.
