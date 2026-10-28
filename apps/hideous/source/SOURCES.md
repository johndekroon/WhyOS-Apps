# Corresponding source: HIDeous

HIDeous is our own program, written for the badge. The files are copied
from why2025-linux `sdapps/hideous/`, `patches/buildroot/overlay-src/`
and `libwb/` at commit `c8cfa09`:

| File | What |
|---|---|
| `hideous.c` | the app: the list, the editor, the keyboard and mouse reports |
| `test_hideous.py` | its host test: snippets added, saved, typed and read back |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `usbkbd.h` | the badge's keys as USB boot-keyboard reports (shared with the menu's keyboard mode), and the mouse's buttons |
| `libwb/` | why2025-linux's drawing library `libwb/` (`wb.h` and its parts: the design tokens, fonts and icons, the frontpanel's scene, glass, pop-ups, drawing only what changed; the status bar `wbstatus.h` with `lora_state.h`; `keyrep.h`, which held keys repeat), as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: why2025-linux `libwb/gen/fonts/`) |

Build: `sdapps-build.sh hideous` ([toolchain/](../../../toolchain/README.md)),
with `hideous.c` and `app.conf` in `sdapps/hideous/`, `libwb/` next to
`sdapps/` and `usbkbd.h` in `patches/buildroot/overlay-src/`, as in
why2025-linux. On a Linux or macOS host, `python3 test_hideous.py` (in
that layout) builds and tests it.
