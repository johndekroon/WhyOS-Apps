# Corresponding source: HIDeous

HIDeous is our own program, written for the badge. The files are copied
from why2025-linux `sdapps/hideous/` and `patches/buildroot/overlay-src/`
at commit `e030994`:

| File | What |
|---|---|
| `hideous.c` | the app: the list, the editor, the keyboard and mouse reports |
| `test_hideous.py` | its host test: snippets added, saved, typed and read back |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `usbkbd.h` | the badge's keys as USB boot-keyboard reports (shared with the menu's keyboard mode), and the mouse's buttons |
| `wbdraw.h`, `home_assets.h`, `home-assets/wb_tokens.h` | the menu's drawing code, fonts and icons, and the design tokens (also in `apps/calc/source`) |
| `wbstatus.h`, `lora_state.h` | the menu's status bar and the LoRa state it shows |

Build: `sdapps-build.sh hideous` ([toolchain/](../../../toolchain/README.md)),
with `hideous.c` and `app.conf` in `sdapps/hideous/` and the other files
in `patches/buildroot/overlay-src/` of why2025-linux. On a Linux or macOS
host, `python3 test_hideous.py` (next to those files) builds and tests it.
