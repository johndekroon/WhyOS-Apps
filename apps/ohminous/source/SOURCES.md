# Corresponding source: OHMinous

OHMinous is our own program, written for the badge. The files are
copied from why2025-linux `sdapps/ohminous/` and
`patches/buildroot/overlay-src/` at commit `0161ded`:

| File | What |
|---|---|
| `ohminous.c` | the app: the start screen, the six tools, the pop-ups, the log, the settings |
| `hw.c`, `hw.h` | the badge's header pins: one GPIO uAPI v2 request, the software logic analyzer, I2C on J7, the software UART, the WS2812 LEDs |
| `bp.c`, `bp.h` | a Bus Pirate 6 on the bottom port, taken over from the menu's Bus Pirate screens (`home.c`) |
| `test_ohminous.py` | its host test, with the hardware simulated |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `wbdraw.h`, `home_assets.h`, `home-assets/wb_tokens.h` | the menu's drawing code, fonts and icons, and the design tokens |
| `wbstatus.h`, `lora_state.h` | the menu's status bar and the LoRa state it shows |
| `keyrep.h` | which held keys repeat |

Build: `sdapps-build.sh ohminous` ([toolchain/](../../../toolchain/README.md)),
with `ohminous.c`, `hw.*`, `bp.*` and `app.conf` in `sdapps/ohminous/`
and the other files in `patches/buildroot/overlay-src/` of
why2025-linux. On a Linux or macOS host, `python3 test_ohminous.py`
(next to these files) builds and tests it.
