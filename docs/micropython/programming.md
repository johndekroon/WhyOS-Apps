# Programming for the badge

## The text window

A program started from the menu, from Files or from Write runs in a text
window of **73 columns by 25 rows**, below the badge's status bar.
`print()` and `input()` work as usual. The window understands the usual
terminal escape codes (a VT100/ANSI subset), so a program can use colours,
place text anywhere and redraw parts of the screen.

| Code | Effect |
|---|---|
| `\x1b[2J` | Clear the window |
| `\x1b[H`, `\x1b[5;10H` | Cursor to the top left, to row 5 column 10 (counting from 1) |
| `\x1b[K` | Clear to the end of the line |
| `\x1b[31m` .. `\x1b[37m` | Text colour: red, green, yellow, blue, magenta, cyan, white |
| `\x1b[91m` .. `\x1b[97m` | The bright versions |
| `\x1b[41m` .. `\x1b[47m`, `\x1b[101m` .. `\x1b[107m` | Background colours |
| `\x1b[1m`, `\x1b[4m`, `\x1b[7m` | Bold, underline, reverse |
| `\x1b[0m` | Back to normal |
| `\x1b[?25l`, `\x1b[?25h` | Hide and show the cursor |

The colours are the badge's own palette. Besides ASCII the window shows box
drawing characters (`─ │ ┌ ┐ └ ┘ ├ ┤`) and a few symbols; other characters
appear as `?`.

```python
import time

def at(row, col, text):
    print("\x1b[%d;%dH%s" % (row, col, text), end="")

print("\x1b[2J\x1b[?25l", end="")
at(2, 5, "\x1b[96m┌──────────────┐\x1b[0m")
at(3, 5, "\x1b[96m│\x1b[0m  Countdown   \x1b[96m│\x1b[0m")
at(4, 5, "\x1b[96m└──────────────┘\x1b[0m")
for n in range(10, -1, -1):
    at(6, 11, "\x1b[93m%3d\x1b[0m" % n)
    time.sleep(1)
at(8, 1, "\x1b[?25h")
```

Graphics (pixels, images, the badge's own fonts and menus) cannot be
drawn from Python yet.

## Reading keys

`input()` reads a line: the user types and presses Enter. For a game or a
menu you want every key as it is pressed, without Enter. That needs the
terminal in raw mode. Save this as `keys.py` next to your program (or in
`lib/`, so every program can use it):

```python
# keys.py: read single keys from the badge's keyboard
import sys, termios, select

UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
ENTER, BACKSPACE, TAB, ESC = "\r", "\x7f", "\t", "\x1b"
DELETE = "\x1b[3~"                                 # Fn+Backspace
SQUARE, TRIANGLE, CROSS, CIRCLE = "\x1bOP", "\x1bOQ", "\x1bOR", "\x1bOS"
CLUB, DIAMOND = "\x1b[15~", "\x1b[17~"
CTRL_C = "\x03"


class Keys:
    def __enter__(self):
        self.old = termios.tcgetattr(0)
        termios.setraw(0)
        self.p = select.poll()
        self.p.register(0, select.POLLIN)
        return self

    def __exit__(self, *exc):
        termios.tcsetattr(0, termios.TCSANOW, self.old)

    def get(self, timeout_ms=-1):
        """The next key as a string, or None when none came within timeout_ms
        (-1: wait for ever)."""
        if not self.p.poll(timeout_ms):
            return None
        k = sys.stdin.read(1)
        if k == ESC and self.p.poll(20):           # Esc, or the start of a code
            k += sys.stdin.read(1)                 # "[" or "O"
            while self.p.poll(20):
                c = sys.stdin.read(1)
                k += c
                if c.isalpha() or c == "~":
                    break
        return k
```

Letters, digits and punctuation arrive as themselves, Ctrl with a letter
as a control character (`"\x03"` for Ctrl-C). The symbol keys ■ ▲ ✕ ● ♣ ◆
arrive as the codes of F1 to F6.

Things to know about raw mode:

- **Ctrl-C no longer stops the program**: it arrives as `"\x03"`. Handle it
  yourself, as in the example below. The logo key always closes the
  window.
- **A new line needs `"\r\n"`**: in raw mode `"\n"` only moves down.
  Moving the cursor with escape codes avoids the question.
- **`with Keys()` puts the terminal back** when the block ends, also after
  an error.

To see what a key sends (in ● Terminal and over SSH some keys send other
codes than in the window):

```python
from keys import Keys
with Keys() as keys:
    while (k := keys.get()) != "q":
        print(repr(k), end="\r\n")
```

### A small game loop

Move a `@` with the arrow keys, `q`, Esc or Ctrl-C to stop:

```python
from keys import Keys, UP, DOWN, LEFT, RIGHT, ESC, CTRL_C

W, H = 73, 25
x, y = W // 2, H // 2

def at(row, col, text):
    print("\x1b[%d;%dH%s" % (row, col, text), end="")

print("\x1b[2J\x1b[?25l", end="")
with Keys() as keys:
    while True:
        at(y + 1, x + 1, "\x1b[93m@\x1b[0m")
        k = keys.get(100)                # None after 100 ms: time for animation
        if k in ("q", ESC, CTRL_C):
            break
        at(y + 1, x + 1, " ")
        if k == UP:
            y = max(0, y - 1)
        elif k == DOWN:
            y = min(H - 1, y + 1)
        elif k == LEFT:
            x = max(0, x - 1)
        elif k == RIGHT:
            x = min(W - 1, x + 1)
print("\x1b[2J\x1b[H\x1b[?25h", end="")
```

## Stopping a program

| How | What happens |
|---|---|
| The program ends | From the menu or Files the window closes when the program ends without an error; after an error it stays open with the message until Enter or Esc. From Write the window always stays open until Enter or Esc |
| Ctrl-C | `KeyboardInterrupt` in the program (not in raw mode, see above). Catch it with `try ... except KeyboardInterrupt` to clean up |
| Logo key | Closes the window and ends the program at once; `finally` blocks do not run |
| `sys.exit(n)` | Ends with status `n`; `0` is success |

## Saving data

Open files with a full path; the current directory depends on how the
program was started.

| Where | Path | Good for |
|---|---|---|
| SD card | `/mnt/sd/badge/appdata/micropython/` (or a folder of your own under `/mnt/sd/badge/appdata/`) | Almost everything: there is room |
| The badge's flash | `/root/` | Small settings that should survive without a card. The flash is small (a few MB for everything) and wears with writing |
| Memory | `/tmp/` | Temporary files; gone after a restart |

A settings file with `json`:

```python
import json

PATH = "/mnt/sd/badge/appdata/micropython/game.json"

def load():
    try:
        with open(PATH) as f:
            return json.load(f)
    except OSError:                      # not there yet
        return {"high_score": 0}

def save(data):
    with open(PATH, "w") as f:
        json.dump(data, f)

data = load()
data["high_score"] = max(data["high_score"], 42)
save(data)
```

The badge can reset when its USB cable is pulled while it runs on the
battery. Close files (a `with` block does) right after writing, so little
is lost when that happens.

## Errors and debugging

- An uncaught error prints a traceback with file and line, and the window
  stays open so you can read it.
- `python -i script.py` (or Files > Open with > MicroPython) leaves you at
  the prompt after the script, with its variables, to look around.
- What a program prints when started from the menu is also written to
  `/tmp/app-<folder>.log`.

## Your own modules and packages

- **Next to your script:** the script's own folder is searched first, so
  `import helpers` finds `helpers.py` in the same folder.
- **For every program:** put modules in
  `/mnt/sd/badge/appdata/micropython/lib/`.
- **From micropython-lib:** `mip` installs packages into that `lib/`. It
  needs Wi-Fi.

  ```
  python -m mip install datetime
  python -m mip install logging
  python -m mip install github:user/repo/path/module.py
  ```

  Or in the prompt: `import mip; mip.install("datetime")`. `pip` does not
  exist, and packages from PyPI for CPython generally do not work.
- The modules built into MicroPython (`asyncio`, `requests` and the others
  in [python.md](python.md)) win over a module with the same name in
  `lib/`.

## Doing several things at once

There are no threads. `asyncio` runs several tasks in one program:

```python
import asyncio

async def blink():
    on = False
    while True:
        on = not on
        print("\x1b[1;1H" + ("\x1b[92m*\x1b[0m" if on else " "), end="")
        await asyncio.sleep_ms(500)

async def count():
    for i in range(10):
        print("\x1b[3;1Hcount", i, end="")
        await asyncio.sleep(1)

async def main():
    asyncio.create_task(blink())
    await count()

print("\x1b[2J", end="")
asyncio.run(main())
```

A task gives the others a turn at every `await`. A long computation
without `await`, or a blocking `input()`, holds up all tasks.

## Memory

A program has **2 MB** for its objects. That is plenty for most scripts;
a `MemoryError` means it ran out.

- Give a program more: `MICROPY_HEAP=4M python big.py`. All programs on
  the badge share about 13 MB, so take only what you need.
- Reuse buffers (`bytearray`, `readinto()`), and build long strings with
  `"".join(parts)` instead of `+=` in a loop.
- `import gc; gc.collect(); print(gc.mem_free())` shows what is left.

## Faster code

Python code runs as bytecode. Two decorators compile a function to machine
code for the badge's processor, which is useful for tight loops:

```python
import micropython

@micropython.native            # the same Python, compiled
def total(n):
    s = 0
    for i in range(n):
        s += i
    return s

@micropython.viper             # integers as machine words: faster still
def total3(n: int) -> int:
    s = 0
    for i in range(n):
        s += i * 3
    return s
```

`viper` integers wrap at 32 bits, and its raw pointers (`ptr8`, `ptr32`)
can crash the whole badge when they point to the wrong place.

## A tile in the menu

A program can get its own row under Apps or Games. The menu lists every
folder in `/mnt/sd/badge/apps/` that has an `app.conf`. For a program
`hello`:

`/mnt/sd/badge/apps/hello/app.conf`:

```
NAME="Hello"
INFO="My first program"
CATEGORY=apps
ICON=terminal
EXEC=start
TERMINAL=close
```

`/mnt/sd/badge/apps/hello/start`:

```sh
#!/bin/sh
exec /root/.whyos/bin/python /mnt/sd/badge/apps/hello/main.py "$@"
```

and your program as `/mnt/sd/badge/apps/hello/main.py`. Then, in ●
Terminal or over SSH:

```
chmod +x /mnt/sd/badge/apps/hello/start
```

The menu shows "Hello" under Apps the next time it lists the apps.

| Setting | Choices |
|---|---|
| `CATEGORY` | `apps`, `games` |
| `ICON` | `terminal`, `games`, `sensors`, `wifi`, `lora`, `led`, `chip`, `keyboard`, `text`, `sun`, `bolt`, `palette` and more (see why2025-linux `docs/APPS.md`) |
| `TERMINAL` | `close`: the window closes when the program ends without an error. `1`: the window always stays open until Enter or Esc, for a program whose last output should stay readable |
| `OPENS_ALSO` | Extensions, such as `"txt csv"`: Files then offers your program under Open with for such files; `ARGS="%f"` passes the file, and `"$@"` in `start` hands it to the program as `sys.argv[1]` |

Delete the folder to remove the tile. Keep your program's data out of the
app folder, in `/mnt/sd/badge/appdata/hello/` for example, so you can
replace the program without losing it.
