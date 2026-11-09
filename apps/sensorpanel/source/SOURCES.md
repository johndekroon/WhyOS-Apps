# Corresponding source: Sensor panel

The sensor panel is a program of why2025-linux, rebuilt on the WhyOS SDK.
The files are copied from WhyOS at commit `59c4625`:

| File | What |
|---|---|
| `sensorpanel.c` | the app: its canvas, the readings and the graph, described for the SDK |
| `tests/` | its tests (`test_sensorpanel.py`), with the sensors as files in a made-up sysfs |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `sdk/` | WhyOS's SDK, `sdk/`: `include/` what the app is built with (`whyos/dev.h` reads the sensors), `testing/` the test framework, `README.md` the guide |
| `libwb/` | WhyOS's drawing library `libwb/`, which the SDK draws with, as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: WhyOS `libwb/gen/fonts/`) |
| `sandbox/` | `policy.h` and `test/sbsim.*` from WhyOS `sandbox/`: what the tests run the app under |

Build: `sdapps-build.sh sensorpanel` ([toolchain/](../../../toolchain/README.md)),
with the app's files in `sdapps/sensorpanel/`, and `sdk/`, `libwb/` and
`sandbox/` next to `sdapps/`, as in WhyOS. On a Linux or macOS host,
`python3 -m pytest sdapps/sensorpanel/tests` (in that layout) tests it.
