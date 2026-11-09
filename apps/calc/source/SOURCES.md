# Corresponding source: Calculator

The calculator is our own program, written for the badge on the WhyOS SDK.
The files are copied from WhyOS at commit `3cb2349`:

| File | What |
|---|---|
| `calc.c` | the app: its entry screen, keys and setting, described for the SDK |
| `sums.c`, `sums.h` | its own logic: a sum worked out, numbers as text |
| `tests/` | its tests: `unit/test_sums.c` the sums on their own, `test_calc.py` the app |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |
| `sdk/` | WhyOS's SDK, `sdk/`: `include/` what the app is built with, `testing/` the test framework, `README.md` the guide |
| `libwb/` | WhyOS's drawing library `libwb/`, which the SDK draws with, as built; `gen/` the scripts that made its data, with the fonts' licences (the fonts themselves: WhyOS `libwb/gen/fonts/`) |
| `sandbox/` | `policy.h` and `test/sbsim.*` from WhyOS `sandbox/`: what the tests run the app under |

Build: `sdapps-build.sh calc` ([toolchain/](../../../toolchain/README.md)),
with the app's files in `sdapps/calc/`, and `sdk/`, `libwb/` and
`sandbox/` next to `sdapps/`, as in WhyOS. On a Linux or macOS host,
`python3 -m pytest sdapps/calc/tests` (in that layout) tests it.
