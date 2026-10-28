# Corresponding source: OHMinous

OHMinous is our own program, written for the badge. The files are
copied from why2025-linux `sdapps/ohminous/` and
`libwb/` at commit `c8cfa09`:

| File | What |
|---|---|
| `ohminous.c` | the app: the start screen, the six tools, the pop-ups, the log, the settings |
| `hw.c`, `hw.h` | the badge's header pins: one GPIO uAPI v2 request, the software logic analyzer, I2C on J7, the software UART, the WS2812 LEDs |
| `bp.c`, `bp.h` | a Bus Pirate 6 on the bottom port, taken over from the menu's Bus Pirate screens (`home.c`) |
| `test_ohminous.py` | its host test, with the hardware simulated |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `libwb/` | why2025-linux's drawing library `libwb/` (`wb.h` and its parts: the design tokens, fonts and icons, the frontpanel's scene, glass, pop-ups, drawing only what changed; the status bar `wbstatus.h` with `lora_state.h`; `keyrep.h`, which held keys repeat), as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: why2025-linux `libwb/gen/fonts/`) |

Build: `sdapps-build.sh ohminous` ([toolchain/](../../../toolchain/README.md)),
with `ohminous.c`, `hw.*`, `bp.*` and `app.conf` in `sdapps/ohminous/`
and `libwb/` next to `sdapps/`, as in why2025-linux. On a Linux or macOS
host, `python3 test_ohminous.py` (in that layout, or next to these files
with `libwb/`) builds and tests it.
