# Corresponding source: Bus Pirate

The Bus Pirate screens are part of the badge's start menu; this app is the
menu's source built with `-DBP_APP`, which starts on those screens and ends
where the menu would go back to its home screen. The files are copied from
why2025-linux `patches/buildroot/overlay-src/` at commit `40e60f6`:

| File | What |
|---|---|
| `home.c` | the start menu, with the Bus Pirate's screens |
| `settings.h` | the general settings it reads |
| `wbdraw.h`, `home_assets.h`, `home-assets/wb_tokens.h` | drawing, fonts and icons, design tokens (also in `apps/imgview/source`) |

Build: `sdapps-build.sh buspirate` ([toolchain/](../../../toolchain/README.md)),
with these files in `patches/buildroot/overlay-src/` of why2025-linux.
