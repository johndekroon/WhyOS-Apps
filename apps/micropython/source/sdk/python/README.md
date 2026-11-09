# The WhyOS SDK in Python

The store's MicroPython has the SDK built in as the module `whyos`: the same screens, actions,
settings and log as in C (`../README.md`), described in Python. The SDK runs the loop and draws in
C; Python describes the screens and handles what their keys do.

```python
import whyos

MODES = [(0xFFFFFF, 80), (0xFFFFFF, 30), (0xFF0000, 80)]
light = whyos.Colour(keys_ms=2000)


def show(mode):
    mode %= 3
    whyos.set("mode", mode)
    light.colour, light.brightness = MODES[mode]


light.action(whyos.OPTIONS, "White 100%", "White at full brightness", lambda: show(0))
light.action(whyos.MAIN, "White 50%", "White at half brightness", lambda: show(1))
light.action(whyos.SECOND, "Red 100%", "Red at full brightness", lambda: show(2))
light.action(whyos.ARROWS, None, "Step through the modes", lambda step: show(int(whyos.get("mode", 0)) + step))
show(int(whyos.get("mode", 0)))
whyos.run("Flashlight", light, keep_on=True, about="The whole screen as a light.")
```

That is the flashlight, as the C one: the key bar for two seconds, the help from the actions,
the log and settings screens, Esc that closes, the mode kept. `examples/` has it and a notes app
with a list, an entry screen, a menu and a setting.

## Screens

| Call | Screen | Fields |
|---|---|---|
| `whyos.Colour(**fields)` | The whole screen one colour | `colour` (0xRRGGBB), `brightness` (10 to 80), `keys_ms` |
| `whyos.List(**fields)` | Rows | `rows` (a list of strings, or of `(label, value, sub)`), `choose(i)` on Enter, `menu` (`[(word, fn(i)), ...]` for the square key), `focus` |
| `whyos.Editor(**fields)` | Text over several lines | `max` (bytes), `text` (read it back after editing), `placeholder` |
| `whyos.Entry(**fields)` | A line typed, worked out on Enter | `evaluate(line)` returns the result as text, or raises with what is wrong; `preview(line)` while typing; `placeholder`; `right` |

Every screen also has `title`, `eyebrow`, `about` (the help's sentence), `title_right` (a value
right of the title) and `update` (a function called before every frame, to put the app's data in
the fields). A field can be changed at any time; the SDK draws what changed after every key,
tick or log line.

`screen.action(key, word, help, fn)`: `fn()` on that key, `word` in the key bar (`None`: only in
the help), `help` the help's line. For `whyos.ARROWS`, `whyos.LEFTRIGHT` and `whyos.UPDOWN`,
`fn(step)` gets -1 or +1. An action without `fn` only describes a key the screen handles itself
(`whyos.ENTER` in a list or an entry). The keys: `whyos.OPTIONS` (■), `MAIN` (▲), `SECOND` (✕),
`ENTER`, `TAB`, `BACKSPACE`, `DELETE`, `SPACE`, `UP`, `DOWN`, `LEFT`, `RIGHT`, and
`whyos.char("r")` for a character.

## The app

```python
whyos.run(name, screen, about=None, keep_on=False, settings=None, tick=None, tick_ms=0, quit=None)
```

runs the app from its first screen until Esc there, the logo key or `whyos.quit()`.
`whyos.push(screen)` opens another screen on top, Esc comes back (`whyos.pop()` from code).

Settings, shown on the clover's screen and kept in `APP_CONFIG/settings`:

```python
settings=[("upper", "Capitals", whyos.SWITCH, "0"),
          ("unit", "Unit", whyos.CHOICE, "c", "c=Celsius|f=Fahrenheit"),
          ("speed", "Speed", whyos.SLIDER, "5", (1, 10, 1), "How fast")]
```

A List with `fields=[...]` (the same tuples) is a form: its rows are those fields, changed as
settings are and kept the same way, for an app's own values ("New item": a name, a size).

`whyos.get(key, default=None)` reads one as text (the schema's default when none is set),
`whyos.set(key, value)` keeps it at once, `whyos.log(text)` adds a line to the circle's log
screen and to the app's log file.

Questions, text and messages, as in C:

```python
whyos.ask("Clear it?", "Its notes go.", ["Keep", "Clear"], done, danger=1)   # done(i), -1 on Esc
whyos.ask_text("Rename", "A new name.", name, done)    # done(text): None closes, a message keeps it open
whyos.message("Saved")
```

A screen's `dirty = True` makes Esc ask "Discard changes?" first. `busy = "Typing"` shows a
running action in a pop-up (with a bar while `progress` is 0 to 1000), takes no key but Esc, and
calls `abort()` on Esc; the app sets `busy = None` when it is done. A setting of kind `whyos.TEXT`
is typed in the text pop-up.

An exception in a callback is printed to the app's log and the app goes on; in `evaluate` its
message is shown as what is wrong with the line. `SystemExit` ends the app.

## A Python app in the menu

`/mnt/sd/badge/apps/<id>/` with the program as `main.py` and an `app.conf` without `TERMINAL`
(the app draws the panel) that runs it with the store's MicroPython:

```
NAME="Flashlight (Python)"
INFO="a white or red light"
CATEGORY=apps
ICON=sun
EXEC=/mnt/sd/badge/apps/micropython/micropython
ARGS="-X heapsize=512K %d/main.py"
```

`%d` is the app's folder. MicroPython itself, not the `python` command or a start script: the
command runs as the MicroPython app, with its `APP_ID` and places, so the settings and the log
would be MicroPython's; and a start script is one more program to load (a 636 KB busybox), which
on a badge with little kernel memory left was once ended by the OOM killer while it loaded
MicroPython. `templates/python` has both files and a test; `sdk/tools/whyos-sdk new python <dir>`
copies it.

## Testing

The same test framework as for C apps (`../testing/whyos_testing.py`), with `PyApp` for a
script:

```python
from whyos_testing import PyApp

def test_red(tmp_path):
    app = PyApp("main.py", "flashlight", tmp=tmp_path)
    app.advance(2).press("✕")
    assert app.screen["colour"] == "#ff0000" and app.backlight == 80
```

It runs the script under a MicroPython for the computer whose `whyos` module meets the
sandbox's simulation, as a C app does; `../tools/micropython-sbsim.sh` builds that MicroPython
once (the same version and patches as the badge's), in `~/.cache/whyos-sdk`.
`python3 -m pytest sdk/python/tests` tests the module with the examples.

## Limits

- No drawing from Python yet: a screen is one of the kinds above, drawn by the SDK.
- Up to 16 actions per screen, 16 settings, and 128 actions in all.
- Each key costs the C drawing (about 150 ms for a focus move on the badge) plus the Python
  callback.
