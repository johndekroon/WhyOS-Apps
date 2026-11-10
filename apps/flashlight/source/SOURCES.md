# Corresponding source: Flashlight

The flashlight is our own program, written for the badge on the WhyOS SDK.
The files are copied from WhyOS at commit `ca53300`:

| File | What |
|---|---|
| `flashlight.c` | the app: its colour screen, the modes and their keys, described for the SDK |
| `tests/` | its tests (`test_flashlight.py`) |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `sdk/` | WhyOS's SDK, `sdk/`: `include/` what the app is built with, `testing/` the test framework, `README.md` the guide |
| `libwb/` | WhyOS's drawing library `libwb/`, which the SDK draws with, as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: WhyOS `libwb/gen/fonts/`) |
| `sandbox/` | `policy.h` and `test/sbsim.*` from WhyOS `sandbox/`: what the tests run the app under |

Build: `sdapps-build.sh flashlight` ([toolchain/](../../../toolchain/README.md)),
with the app's files in `sdapps/flashlight/`, and `sdk/`, `libwb/` and
`sandbox/` next to `sdapps/`, as in WhyOS. On a Linux or macOS host,
`python3 -m pytest sdapps/flashlight/tests` (in that layout) tests it.
