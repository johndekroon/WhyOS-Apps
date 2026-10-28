# What Python you get

MicroPython 1.29 is Python 3 with a smaller standard library. Code you
write for Python on your computer usually runs as it is, as long as it
sticks to the language and the modules below.

## The language

Everything you use day to day is there: functions, classes and
inheritance, exceptions, generators, list/dict/set comprehensions,
f-strings, `:=`, `async`/`await`, `with`, decorators, `*args`/`**kwargs`,
integers of any size, floats (double precision), Unicode strings,
`bytes`/`bytearray`/`memoryview`. Type annotations are allowed, but there
is no `typing` module and nothing checks them.

The differences that come up most:

| In Python on a computer | Here |
|---|---|
| A large standard library | A small one (below); more from micropython-lib with `mip` |
| `pip install` from PyPI | `mip install` from micropython-lib, GitHub, GitLab or Codeberg |
| `threading` | `asyncio` |
| `subprocess` | `os.system("command")` returns the exit status, output goes to the window |
| `os.path`, `pathlib`, `shutil` | Not built in; `mip install os-path`, `pathlib` or `shutil`, or plain strings: `folder + "/" + name` |
| `time.strftime`, `datetime` | Format with `"%02d:%02d" % (h, m)`, or `mip install datetime` |
| `random.shuffle`, `random.sample` | Not built in; see below |
| `re` match `.groups()`, `.span()`, `re.finditer` | Not built in; use `.group(1)`, `.group(2)` ... |
| `collections.defaultdict`, `Counter` | `mip install collections-defaultdict`; a `dict` with `.get(k, 0) + 1` |
| `sys.getsizeof`, `sys.settrace` | Not there |
| `dataclasses`, `enum`, `typing` | Not there; `collections.namedtuple` and plain classes |
| `sqlite3`, `csv` | Not there; `json` files, or `line.split(",")` |
| `input()`, `print()` | The same |

Shuffling a list without `random.shuffle`:

```python
import random

def shuffle(items):
    for i in range(len(items) - 1, 0, -1):
        j = random.randint(0, i)
        items[i], items[j] = items[j], items[i]
```

Some smaller differences in behaviour (for example in exception details
and in some special methods) are listed in MicroPython's
[differences from CPython](https://docs.micropython.org/en/v1.29.0/genrst/index.html).

## Modules in this build

`help('modules')` at the prompt prints the list on your badge.

### Standard library (MicroPython versions)

| Module | Has |
|---|---|
| `array` | `array.array` |
| `asyncio` | `run`, `create_task`, `gather`, `sleep`, `sleep_ms`, `wait_for`, `Event`, `Lock`, streams (`open_connection`, `start_server`) |
| `binascii` | `hexlify`, `unhexlify`, base64 (`a2b_base64`, `b2a_base64`), `crc32` |
| `cmath` | Complex maths |
| `collections` | `namedtuple`, `OrderedDict`, `deque` |
| `errno` | Error numbers, as in `OSError.errno` |
| `gc` | `collect`, `mem_free`, `mem_alloc` |
| `hashlib` | `sha256`, `sha1`, `md5` |
| `heapq` | `heappush`, `heappop`, `heapify` |
| `io` | `open`, `StringIO`, `BytesIO` |
| `json` | `dumps`, `loads`, `dump`, `load` |
| `math` | The usual functions, `factorial`, `isclose`, `gamma`, `erf`; not `gcd`, `isqrt`, `comb`, `prod` |
| `os` | `listdir`, `mkdir`, `remove`, `rename`, `rmdir`, `stat`, `statvfs`, `getcwd`, `chdir`, `getenv`, `putenv`, `system`, `urandom`; not `os.path`, `os.uname` |
| `platform` | `platform()` |
| `random` | `random`, `randint`, `randrange`, `choice`, `uniform`, `getrandbits`, `seed` |
| `re` | `compile`, `match`, `search`, `sub`; `split` only on a compiled pattern: `re.compile(",").split(text)` |
| `select` | `poll` (not `select.select`) |
| `socket` | TCP and UDP, clients and servers, `getaddrinfo` |
| `ssl` | `wrap_socket`, `SSLContext` (TLS through Mbed TLS) |
| `struct` | `pack`, `unpack`, `pack_into`, `unpack_from`, `calcsize` |
| `sys` | `argv`, `path`, `exit`, `stdin`, `stdout`, `stderr`, `print_exception`, `platform` (`"linux"`) |
| `termios` | Raw keyboard input ([programming.md](programming.md)) |
| `time` | `time`, `localtime`, `gmtime`, `mktime`, `sleep`, `sleep_ms`, `ticks_ms`, `ticks_diff` |

### Also built in

| Module | Has |
|---|---|
| `requests` | `get`, `post`, `put`, `delete`, `head`: HTTP and HTTPS ([badge.md](badge.md)) |
| `meshcore` | MeshCore messages over LoRa, for bots ([badge.md](badge.md)); from MicroPython 1.29.0 build 2 |
| `setting` | The badge's settings, such as the name ([programming.md](programming.md)); from MicroPython 1.29.0 build 2 |
| `ics` | Calendars from an .ics link or file, repeats worked out ([badge.md](badge.md)); from MicroPython 1.29.0 build 2 |
| `xml.etree.ElementTree` | Reading and writing XML, as in CPython ([badge.md](badge.md)); from MicroPython 1.29.0 build 2 |
| `mip` | The package installer |
| `argparse` | Command-line options for your scripts |
| `deflate` | gzip and zlib compression and decompression (`DeflateIO`) |
| `cryptolib` | AES in ECB and CBC mode |
| `framebuf` | Drawing into a pixel buffer (no way to show it on the screen yet) |
| `micropython` | `const`, the `native` and `viper` decorators, `mem_info` |
| `uctypes` | C structures over raw memory (careful: a wrong address can hang the badge) |
| `machine` | Little more than its name: no `Pin`, `I2C`, `SPI` ([badge.md](badge.md)) |

### Not available

| Module | Instead |
|---|---|
| `threading`, `_thread` | `asyncio` |
| `network` | Wi-Fi is set up in Settings; sockets just work |
| `bluetooth`, `machine.Pin`, `I2C`, `SPI`, `UART` | See [badge.md](badge.md) |
| `ctypes`/`ffi`, and micropython-lib's `unix-ffi` packages | Not possible on this badge |

## Packages from micropython-lib

Pure-Python packages from [micropython-lib](https://github.com/micropython/micropython-lib)
install with `python -m mip install <name>`, for example `datetime`,
`logging`, `os-path`, `pathlib`, `shutil`, `itertools`, `functools`,
`string`, `textwrap`, `unittest`, `base64`, `hmac`, `uuid`, `copy`. They
go to `lib/` in your scripts folder. Not every package has been tried on
the badge.
