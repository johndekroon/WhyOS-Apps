# Corresponding source: Sensor panel

`sensorpanel.c` is the program, copied from why2025-linux
`sdapps/sensorpanel/` at commit `c8cfa09` (it was `overlay-src/sensorpanel.c`
of the OS image before). `libwb/` is why2025-linux's drawing library
(`libwb/` at the same commit: the design tokens, fonts and icons, the
frontpanel's scene, glass, the status bar, drawing only what changed), as
built; `gen/` holds the scripts that made its data, with the fonts'
licences (the fonts themselves: why2025-linux `libwb/gen/fonts/`). The
toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

Build: put `sensorpanel.c` in `sdapps/sensorpanel/` next to
`toolchain/sdapps-build.sh`, `libwb/` next to `sdapps/` (as in
why2025-linux), and run `sdapps-build.sh sensorpanel`.
