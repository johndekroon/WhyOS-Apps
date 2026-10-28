# Language reference

Status: 2026-10-28. The language is MicroPython 1.29's; see the
[official language reference](https://docs.micropython.org/en/v1.29.0/reference/index.html).
This page covers what differs on the badge or matters more there.

## The REPL

The REPL and its keys are in [quickref.md](quickref.md). From the menu it
runs in appterm's window of 73 × 25 characters; Fn+↑ and Fn+↓ scroll back
through the last 300 lines (`why2025-linux/docs/APPS.md`, command-line
apps). Over SSH or the serial console the REPL uses your terminal instead.

Paste mode (Ctrl-E) is the way to enter a block that has its own
indentation: the REPL's automatic indent would otherwise add to it.

## Faster code

Bytecode is MicroPython's default. On the badge the RV32 native code
emitter is built in as well, so the decorators work:

```python
import micropython

@micropython.native
def total(n):
    s = 0
    for i in range(n):
        s += i
    return s

@micropython.viper
def total3(n: int) -> int:
    s = 0
    for i in range(n):
        s += i * 3
    return s
```

- `@micropython.native` compiles a function to machine code: the same
  Python, usually faster, a little larger.
- `@micropython.viper` goes further with machine integers (`int` wraps at
  32 bits) and raw pointers (`ptr8`, `ptr16`, `ptr32`). A pointer to an
  address that is not yours can hang the badge
  ([general.md](general.md), memory).
- `python -X emit=native script.py` compiles the whole script as native
  code.
- `micropython.const(x)` for constants, and `-O1`..`-O3` (or
  `micropython.opt_level()`) to drop asserts and line numbers.
- Not available: the inline assembler (`@micropython.asm_rv32`); it is not
  in this build.

Tested in the simulator (2026-10-12): `native` and `viper` functions give
the right results. How much faster they are on the badge has not been
measured.

On NOMMU Linux the code the emitter writes lands in ordinary memory and
runs from there; nothing special is needed (`why2025-linux/sdapps/README.md`).

## Precompiled .mpy files

MicroPython imports `.mpy` files like `.py` files, and `mip` installs
packages as `.mpy` (the simulator test found `lib/base64.mpy`).

- **Bytecode** `.mpy` files come from `mpy-cross` of the same MicroPython
  version (1.29.0). `sys.implementation._mpy` gives the format this build
  expects.
- **Native code** in a `.mpy` needs `mpy-cross -march=rv32imc` (the badge's
  emitter). The build can load native code from `.mpy` files (derived from
  the build options); not tested.
- Precompiling saves the compiler's memory and start-up time on the badge,
  which helps a large module fit in the 2 MB heap.

## Memory management

The heap is fixed at start (2 MB by default, `MICROPY_HEAP` for another
size), and the badge shares one pool between all programs
([general.md](general.md)). What helps:

- Allocate buffers once and reuse them: `bytearray`, `memoryview`, and the
  `readinto()` methods of files and sockets avoid a new object per read.
- Build strings with a list and `"".join()`, or write pieces to a file or
  socket straight away.
- `gc.collect()` before a large allocation; `micropython.mem_info()` shows
  the heap's state, `gc.mem_free()` the bytes left.
- `micropython.heap_lock()` makes an allocation raise `MemoryError`: a
  check that a loop does not allocate.

## Differences from CPython

MicroPython implements Python 3 with a smaller library; the
[official list of differences](https://docs.micropython.org/en/v1.29.0/genrst/index.html)
applies here unchanged. In practice on the badge:

- Modules are subsets ([library.md](library.md)). `os.path`, `shutil`,
  `datetime`, `logging` and the like come from micropython-lib through
  `mip`, where they exist.
- No threads, no `subprocess`, no `ctypes`/`ffi`.
- Error messages are detailed (as in CPython); tracebacks show file and
  line.
- Integers have arbitrary size; floats are double precision.
- Strings are Unicode. appterm shows ASCII, box drawing and a few
  symbols; other characters appear as "?" there, not in the string.

## Making it an app

A script runs from Files (Open with > MicroPython) or a shell. A menu tile
of its own needs a store package with an `app.conf`-style launch entry;
the format is in [package.md](../package.md). There is no example of a
Python app in the store yet.
