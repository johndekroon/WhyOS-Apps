# Corresponding source: BARbarious

BARbarious is our own program, written for the badge. The files are
copied from why2025-linux `sdapps/barbarious/` and
`patches/buildroot/overlay-src/`, and its `libwb/`, at commit `3063aba`:

| File | What |
|---|---|
| `barbarious.c` | the app: the list, the show screen, Quick code, the editor and its pop-ups, the settings, the file |
| `symbol.c`, `symbol.h` | the barcodes: QR Code through qrcodegen, our own Data Matrix, Aztec, PDF417, Code 128, Code 39, EAN-13, UPC-A, ITF and Codabar |
| `symbol_tabs.h` | the bar widths of Code 128, Code 39 and Codabar and PDF417's codeword patterns, from zint (BSD-3-Clause, `licenses/zint.txt`) |
| `qrcodegen/` | Project Nayuki's QR Code generator 1.8.0 (MIT), unchanged |
| `test_barbarious.py` | its host test: the screens and the file, and every type decoded with zxing-cpp |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `libwb/` | why2025-linux's drawing library `libwb/` (`wb.h` and its parts: the design tokens, fonts and icons, the frontpanel's scene, glass, pop-ups, drawing only what changed; the status bar `wbstatus.h` with `lora_state.h`; `keyrep.h`, which held keys repeat), as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: why2025-linux `libwb/gen/fonts/`) |
| `power.h` | the power management's key time, which a code on screen keeps fresh |

Build: `sdapps-build.sh barbarious` ([toolchain/](../../../toolchain/README.md)),
with `barbarious.c`, `symbol.*`, `symbol_tabs.h` and `app.conf` in
`sdapps/barbarious/`, `libwb/` next to `sdapps/` and `power.h` in
`patches/buildroot/overlay-src/`, as in why2025-linux. On a Linux or
macOS host, `python3 test_barbarious.py` (in that layout, or next to
these files) builds and tests it; the decoding needs `pip install zxing-cpp pillow`.
