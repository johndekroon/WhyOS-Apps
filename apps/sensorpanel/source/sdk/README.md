# The WhyOS SDK

An app on the badge is its own logic plus a description of its screens. The SDK runs the loop,
draws the screens in the design system "Neon Orbit" (`../libwb/`), and keeps the rules every app
follows: the key bar, the help on the diamond key, the log on the circle, the settings on the
clover, Esc one layer back at a time, the screen kept on, the end on the logo key. The tests of
an app live beside it, not in it.

```c
#include "whyos.h"

static void hello(int arg)
{
	wos_log("Hello from the badge");
}

static const wos_action keys[] = {
	{ WOS_KEY_MAIN, "hello", "Say hello in the log", hello },
	{ 0 }
};

int main(void)
{
	static const wos_app app = { .name = "Hello", .about = "A first app." };
	static wos_screen first = { .kind = WOS_LIST, .actions = keys };

	return wos_run(&app, &first);
}
```

That is a whole app: a screen with its title, a key bar with "▲ hello" and the circle, clover
and diamond, a help pop-up that lists the keys, a log screen with the line in it, a settings
screen that says the app has none, and Esc that closes it. `templates/` has it, and the other
starting points.

## Contents

| Path | What |
|---|---|
| `include/whyos.h` | The one header an app includes, with `-I sdk/include -I libwb` |
| `include/whyos/core.h` | The app's places, small files written safely, settings, the log, the clock, power |
| `include/whyos/keys.h` | The keys by role, the characters they type |
| `include/whyos/ui.h` | The parts libwb does not draw by itself: the app's key bar, a typed line, a card of lines |
| `include/whyos/app.h` | Screens, actions, settings; `wos_run()` |
| `include/whyos/test.h` | The test channel |
| `include/whyos/dev.h` | The badge's sensors: the BME680's environment, the BMI270's acceleration |
| `include/whyos/check.h` | Unit tests for an app's own logic |
| `python/` | The SDK in MicroPython: the module `whyos`, its guide (`python/README.md`), examples and tests |
| `testing/whyos_testing.py` | The test framework (pytest) |
| `templates/` | Starting points: a list app, an entry app, a canvas app |
| `tools/whyos-sdk` | New app from a template, test, screenshots |
| `tools/micropython-sbsim.sh` | MicroPython for this computer, to test Python apps |
| `test/` | The SDK's own tests |

Everything is static and header-only, as libwb is: a program links only what it uses
(`-ffunction-sections`, `--gc-sections`). An app is one C file that includes `whyos.h`, plus any
files with its own logic that do not draw.

## Screens

A screen is a `wos_screen` with a `kind`. The SDK draws it whole: the header (an eyebrow, the
title, a value on the right), the content, the key bar.

| Kind | What it is | The app gives | Example |
|---|---|---|---|
| `WOS_LIST` | Rows: focus, scrolling, the run-on at the end, a scroll bar | `n`, `row(i, &r)`, `choose(i)` on Enter, `adjust(i, step)` on ← →, `menu` for ■ | The settings screen |
| `WOS_ENTRY` | A line typed, worked out on Enter, the earlier lines above it | `evaluate(line, result)`, `preview(line, result)` while typing, `typed(line, cursor, ch)` | `sdapps/calc` |
| `WOS_COLOUR` | The whole screen one colour, at a brightness | `colour`, `brightness`, `keys_ms` | `sdapps/flashlight` |
| `WOS_CANVAS` | An area the app draws itself, with libwb | `paint(x, y, w, h)`, `key(code, ch)`, `scene` | A graph, a game |

`wos_push(&screen)` opens a screen on top; Esc goes back, and on the first screen Esc closes the
app. `update(screen)` runs before every frame: it puts the app's data into the screen's fields
(a value on the right of the title, a row count), so the app does not tell the SDK when to draw.
The SDK draws after every key, every tick and every change in the log, and only the bands of
40 rows that changed (`wb_bands`, `../libwb/README.md`).

A row (`wos_row`) has a label, an optional second line (`sub`), a value on the right in mono, an
icon, and a kind: `WOS_ROW_OPEN` (a chevron: Enter opens something), `WOS_ROW_PLAIN`,
`WOS_ROW_SWITCH` (with `on`), or `WOS_ROW_INFO` (it never takes the focus).

## Actions

An action is a key that does something on a screen: its word in the key bar, its line in the
help, and a function.

```c
static const wos_action keys[] = {
	{ WOS_KEY_ENTER, "work out", "Work the sum out" },	/* describes Enter, no function */
	{ WOS_KEY_OPTIONS, "White 100%", "White at full brightness", show, 0 },
	{ WOS_KEY_TAB, "deg/rad", "Degrees or radians", toggle },
	{ WOS_KEY_ARROWS, 0, "Step through the modes", step },	/* fn gets -1 or +1 */
	{ 0 }
};
```

From that one table the SDK makes the key dispatch, the key bar and the help, so a key cannot be
in the bar and missing from the help:

- The key bar: Enter first, then ■ ▲ ✕ in that order, then other keys with a word, then the
  circle, clover and diamond small and stacked with "log", "settings" and "help", and Esc on
  the right with one of its words: close, back, clear (cancel in a pop-up). What does not fit
  drops out from the end, "Enter open" first. An action without a word is only in the help.
- The help: the screen's title, one or two sentences (`about`), every key with its line, then
  Esc, the circle, clover and diamond and the logo key.
- `WOS_KEY_ARROWS`, `WOS_KEY_LEFTRIGHT` and `WOS_KEY_UPDOWN` are groups: the function gets the
  step.

The circle (log), clover (settings) and diamond (help) belong to the SDK on every screen, and Esc
too. The keys by role are `WOS_KEY_OPTIONS` (■, a context menu), `WOS_KEY_MAIN` (▲, the main
action) and `WOS_KEY_SECOND` (✕, the second), as the design system says when to use them.

Esc peels off one layer: a pop-up closes; an entry's typed text is cleared; a screen goes back;
the first screen closes the app. Fn+Esc is standby, never the app's.

## Settings

```c
static const wos_setting settings[] = {
	{ "angle", "Angle unit", "For sin, cos and tan.", WOS_CHOICE, "deg", "deg=Degrees|rad=Radians",
	  .changed = angle },
	{ "sound", "Sound", 0, WOS_SWITCH, "1" },
	{ "speed", "Speed", 0, WOS_SLIDER, "5", .min = 1, .max = 10 },
	{ 0 }
};
```

The clover's screen shows them as rows: a switch flips on Enter, a choice opens a pop-up list
with the present one ticked, a slider moves with ← →. A change is written at once to
`APP_CONFIG/settings` (`key=value` lines, written whole and synced) and `changed()` is called.
The app reads a setting with `wos_get(key, 0)` (the schema's default when none is set) or
`wos_get_int(key, def)`, and writes one with `wos_set` or `wos_set_int`; a key that is not in
the schema is kept the same way but not shown (the flashlight keeps its mode so).

## The log

`wos_log("text")` adds a line: the circle's screen shows the last 40, the newest at the bottom,
and each goes to stderr too, which `apprun` keeps in `/tmp/app-<id>.log`.

## Sensors

`whyos/dev.h` reads the badge's sensors, found by name so their order does not matter:
`wos_env_read(&e)` the BME680's temperature (centi-°C), humidity (centi-%) and pressure
(deci-kPa), `wos_accel_read(&a)` the BMI270's acceleration in milli-g. Both return 0 when they
read, and look for the device again after a failure. `wos_add_fixed(buf, v, digits, size)` writes
such a value with its decimals. The sensor panel (`../sdapps/sensorpanel`) uses them.

## The rest of core.h

| Call | What |
|---|---|
| `wos_path(place, name, buf, size)` | A path in `WOS_CONFIG`, `WOS_DATA` (on the card), `WOS_CACHE`, `WOS_DIR` (the app's own files) or `WOS_TMP` (`/tmp/app-<id>`), made when missing |
| `wos_read_file`, `wos_write_file` | A small file whole; written to `.new`, synced and renamed, so a reset leaves the old file or the new one |
| `wos_ms()`, `wos_clock_set()` | Monotonic milliseconds; whether NTP has set the wall clock (the badge keeps no time when off) |
| `wos_keep_on()` | The screen stays on, as a key would; `.keep_on = 1` in the app does it every 5 s, never in standby |
| `wos_standby()` | Whether the badge is in standby |
| `wos_backlight(duty)` | The backlight while the app runs, 10 to 80; `apprun` puts the system's back afterwards |
| `wos_copy`, `wos_add`, `wos_add_int`, `wos_int` | Text without stdio |

`.tick` and `.tick_ms` in the app call a function every so often (a clock, a sensor), after which
the SDK draws. `.quit` runs before the end, on Esc, the logo key (SIGTERM from `apprun`) or
`wos_quit()`.

## Testing

An app's program has no test code. Its tests are in `tests/` beside it:

```python
from whyos_testing import App

def test_red(tmp_path):
    app = App(["flashlight.c"], "flashlight", tmp=tmp_path)
    app.advance(2)                      # the first two seconds pass
    app.press("✕")
    assert app.screen["colour"] == "#ff0000"
    assert app.backlight == 80
    assert app.settings == {"mode": "2"}
    app.check()
```

`App` builds the app's own C for the computer under the sandbox's simulation
(`../sandbox/test/sbsim.c`): its system calls meet what they meet on the badge, the sandbox's
rules for every path, the panel, the keypad, standby, the logo key, on a clock that moves only
when the app waits, so ten minutes take a second and every run repeats exactly.

| Part | What |
|---|---|
| `press("Enter", "◆", "Fn+Esc", "x")`, `type("12*7")` | Keys by their names on the badge |
| `advance(seconds)` | The clock moves on |
| `screen` | What the screen shows, as data: kind, eyebrow, title, rows and focus, the line and result, the key bar, an open pop-up with its items or keys, the log, the settings |
| `pixels`, `pixel(x, y)`, `shot(path)` | The panel, and a PNG of it |
| `settings`, `backlight`, `closed`, `report` | APP_CONFIG/settings, the duty set last, whether it ended, the simulation's report |
| `check()` | No access outside the app's places, no torn frame, the screen drawn in bands equal to it drawn whole |
| `env` | `SBSIM_STANDBY="10000-20000"`, `SBSIM_TERM=3000` (the logo key), `PERMISSIONS=...` |

`screen` comes from the SDK's test channel: when `/tmp/app-<id>/test` exists as the app starts
(or `WOS_TEST` names a directory), the SDK writes what the screen shows to `screen` in it after
every frame, as JSON, and takes keys from the FIFO `keys` (a key code per line; `+code` and
`-code` hold and let go of Shift or Fn). Without that directory it opens nothing. On the badge
a test makes the directory over SSH and starts the app from the menu.

The app's logic is tested without a screen in `tests/unit/*.c` with `whyos/check.h`, built with
the logic files and not the app's `main()`; `whyos_testing.unit()` builds and runs one.

```
python3 -m pytest sdapps/calc/tests      # or: sdk/tools/whyos-sdk test sdapps/calc
```

## Building

With the badge's toolchain, as every app in `../sdapps/` (`../BUILDING.md`):

```
riscv32-buildroot-linux-uclibc-gcc -Os -fPIC -static -ffunction-sections -fdata-sections \
    -I sdk/include -I libwb app.c -Wl,--gc-sections -Wl,--no-relax '-Wl,-elf2flt=-r -s 16384' -o app
```

`sdapps/build.sh` does this for the apps in this repository (`sdk_app` in it).

## Limits

- No stdio in the SDK, no floating point in it, no `mmap`: programs are FLAT, NOMMU and
  soft-float (`../sdapps/README.md`, "Building").
- One entry screen per app: its line and earlier lines are the SDK's.
- A list shows up to 64 rows; a pop-up list up to 24 items, without a filter.
- 32 settings of up to 71 characters; 40 log lines of up to 95.
- Pop-ups are drawn in bands, so their glass is not blurred (`../libwb/README.md`).
- The SDK's version is `WOS_SDK_VERSION`; until 1.0 its calls may change.
