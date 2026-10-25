# Corresponding source: BARbarious

BARbarious is our own program, written for the badge. The files are
copied from why2025-linux `sdapps/barbarious/` and
`patches/buildroot/overlay-src/` at commit `5dc7c73`:

| File | What |
|---|---|
| `barbarious.c` | the app: the list, the show screen, Quick code, the editor and its pop-ups, the settings, the file |
| `symbol.c`, `symbol.h` | the barcodes: QR Code through qrcodegen, our own Data Matrix, Aztec, PDF417, Code 128, Code 39, EAN-13, UPC-A, ITF and Codabar |
| `symbol_tabs.h` | the bar widths of Code 128, Code 39 and Codabar and PDF417's codeword patterns, from zint (BSD-3-Clause, `licenses/zint.txt`) |
| `qrcodegen/` | Project Nayuki's QR Code generator 1.8.0 (MIT), unchanged |
| `test_barbarious.py` | its host test: the screens and the file, and every type decoded with zxing-cpp |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `wbdraw.h`, `home_assets.h`, `home-assets/wb_tokens.h` | the menu's drawing code, fonts and icons, and the design tokens |
| `wbstatus.h`, `lora_state.h` | the menu's status bar and the LoRa state it shows |
| `keyrep.h`, `power.h` | which held keys repeat; the power management's key time, which a code on screen keeps fresh |

Build: `sdapps-build.sh barbarious` ([toolchain/](../../../toolchain/README.md)),
with `barbarious.c`, `symbol.*`, `symbol_tabs.h` and `app.conf` in
`sdapps/barbarious/` and the other files in
`patches/buildroot/overlay-src/` of why2025-linux. On a Linux or macOS
host, `python3 test_barbarious.py` (next to these files) builds and
tests it; the decoding needs `pip install zxing-cpp pillow`.
