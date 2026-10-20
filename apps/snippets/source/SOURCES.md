# Corresponding source: Snippets

Snippets is our own program, written for the badge. The files are copied
from why2025-linux `sdapps/snippets/` and `patches/buildroot/overlay-src/`
at commit `d081e6d`:

| File | What |
|---|---|
| `snippets.c` | the app: the list, the editor, the keyboard reports |
| `test_snippets.py` | its host test: snippets added, saved, typed and read back |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `usbkbd.h` | the badge's keys as USB boot-keyboard reports (shared with the menu's keyboard mode) |
| `wbdraw.h`, `home_assets.h`, `home-assets/wb_tokens.h` | the menu's drawing code, fonts and icons, and the design tokens (also in `apps/calc/source`) |
| `wbstatus.h`, `lora_state.h` | the menu's status bar and the LoRa state it shows |

Build: `sdapps-build.sh snippets` ([toolchain/](../../../toolchain/README.md)),
with `snippets.c` and `app.conf` in `sdapps/snippets/` and the other files
in `patches/buildroot/overlay-src/` of why2025-linux. On a Linux or macOS
host, `python3 test_snippets.py` (next to those files) builds and tests it.
