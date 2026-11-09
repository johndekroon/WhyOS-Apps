# Corresponding source: Sensor panel

`sensorpanel.c` is the program, copied from WhyOS
`sdapps/sensorpanel/` at commit `3ef5aae` (it was `overlay-src/sensorpanel.c`
of the OS image before). `libwb/` is WhyOS's drawing library
(`libwb/` at the same commit: the design tokens, fonts and icons, the
frontpanel's scene, glass, drawing only what changed), as
built; `gen/` holds the scripts that made its data, with the fonts'
licences (the fonts themselves: WhyOS `libwb/gen/fonts/`). The
toolchain and the build script are in
[toolchain/](../../../toolchain/README.md).

Build: put `sensorpanel.c` in `sdapps/sensorpanel/` next to
`toolchain/sdapps-build.sh`, `libwb/` next to `sdapps/` (as in
WhyOS), and run `sdapps-build.sh sensorpanel`.
