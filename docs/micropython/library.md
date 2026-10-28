# Library reference

Status: 2026-10-28. Which modules this build has, and where they behave
differently on the badge. For each module's full interface, see the
[official library reference](https://docs.micropython.org/en/v1.29.0/library/index.html);
these pages list only what the badge's build includes.

How this list was made: the build options and the Unix port's
configuration, checked against the names in the program
([README.md](README.md)). "Tested" means the simulator test of 2026-10-12
imports and uses the module; nothing on this page has run on the badge
except a plain script.

`help('modules')` in the REPL prints the list on your own badge.

## Built-in modules

### Python standard libraries and micro-libraries

These are subsets of the CPython module with the same name.

| Module | Tested | Notes for the badge |
|---|---|---|
| `array` | – | `array.array` with the usual type codes |
| `asyncio` | yes | Frozen Python plus the C core `_asyncio`. Event loop, tasks, `sleep_ms`, `gather`, `wait_for`, `Event`, `Lock`, streams (`open_connection`, `start_server`). The way to do several things at once: there are no threads. `uasyncio` is an alias |
| `binascii` | yes | `hexlify`, `unhexlify`, `a2b_base64`, `b2a_base64`, `crc32` |
| `builtins` | yes | All built-in functions and exceptions, with floats (double precision) and integers of any size |
| `cmath` | – | Complex maths |
| `collections` | yes | `namedtuple`, `OrderedDict`, `deque` (with `appendleft` and `popleft`) |
| `errno` | yes | Error numbers (`ENOENT`, `EAGAIN`, `ETIMEDOUT` and more) and `errorcode` |
| `gc` | yes | `collect`, `mem_free`, `mem_alloc`, `enable`, `disable`, `isenabled`, `threshold` |
| `hashlib` | yes | `sha256`, `sha1`, `md5` |
| `heapq` | yes | `heappush`, `heappop`, `heapify` |
| `io` | yes | `open`, `StringIO`, `BytesIO`, `IOBase`, `FileIO`, `TextIOWrapper` |
| `json` | yes | `dump`, `dumps`, `load`, `loads`; `dumps` takes `separators` |
| `math` | yes | The usual functions plus `erf`, `gamma`, `lgamma`, `factorial`, `isclose`; not `gcd`, `isqrt`, `comb`, `prod` |
| `os` | yes | See [os](#os) below |
| `platform` | yes | `platform()`, `python_compiler()`, `libc_ver()`, `processor()` ([general.md](general.md) on the "glibc" in the answer) |
| `random` | yes | `getrandbits`, `randrange`, `randint`, `choice`, `random`, `uniform`, `seed`; not `shuffle` or `sample`. Seeded at import from the kernel |
| `re` | yes | `compile`, `match`, `search`, `sub`, `split`; a match has `group()`. Not built in: `groups()`, `span()`, `start()`, `end()`, `finditer`, `fullmatch` (derived from the build level) |
| `select` | yes | `poll` and `ipoll` only; `select.select()` is switched off in the Unix port. See [select](#select) |
| `socket` | yes | See [socket](#socket) |
| `ssl` | yes | Frozen Python over `tls`. See [ssl and tls](#ssl-and-tls) |
| `struct` | yes | `pack`, `pack_into`, `unpack`, `unpack_from`, `calcsize`; not `iter_unpack` |
| `sys` | yes | `argv`, `path`, `modules`, `exit`, `stdin`, `stdout`, `stderr`, `print_exception`, `exc_info`, `atexit`, `implementation`, `version`, `version_info`, `platform` (`'linux'`), `byteorder` (`'little'`), `maxsize`, `executable`, `ps1`, `ps2`; not `settrace`, `getsizeof` |
| `time` | yes | See [time](#time) |

### MicroPython-specific libraries

| Module | Tested | Notes for the badge |
|---|---|---|
| `cryptolib` | – | `aes(key, mode, iv)` with mode `1` (ECB) or `2` (CBC); CTR and the `MODE_*` names are not built in |
| `deflate` | yes | `DeflateIO` with `RAW`, `ZLIB`, `GZIP` and `AUTO`: decompress and compress |
| `framebuf` | – | `FrameBuffer` in RGB565, GS2/4/8 and mono formats: drawing into a `bytearray`. See [hardware.md](hardware.md) for the screen |
| `machine` | – | Almost empty on Linux: `mem8`, `mem16`, `mem32`, `time_pulse_us`, `idle`, `PinBase`, `Signal`. No `Pin`, `I2C`, `SPI`, `UART`, `ADC`, `PWM`. See [machine](#machine) |
| `micropython` | yes | `const`, `native`, `viper`, `opt_level`, `mem_info`, `qstr_info`, `stack_use`, `heap_lock`, `heap_unlock`, `kbd_intr`, `schedule`, `RingIO` |
| `uctypes` | – | C structures over memory (`struct`, `addressof`, `bytes_at`, `bytearray_at`, `sizeof`). With an address from outside Python it can hang the badge ([general.md](general.md), memory) |
| `vfs` | – | `mount`, `umount` and `VfsPosix` (a Linux directory); no `VfsFat` or `VfsLfs` |
| `websocket` | – | The low-level frame layer that WebREPL uses; WebREPL itself is not in the Unix port |

### Unix-port libraries

| Module | Tested | Notes for the badge |
|---|---|---|
| `termios` | yes (import) | `tcgetattr`, `tcsetattr`, `setraw`: raw keys from a terminal |

## Frozen Python modules

Compiled into the program (`.frozen` on `sys.path`), so they import
without files on the card:

| Module | From | What |
|---|---|---|
| `asyncio`, `uasyncio` | MicroPython | See above |
| `ssl` | micropython-lib | The CPython-like layer over `tls` |
| `requests` | micropython-lib | `get`, `post`, `put`, `delete`, `head`, `request`; a `Response` with `status_code`, `text`, `content`, `json()`. Untested on its own; `mip` uses it |
| `mip` | micropython-lib | The package installer (`mip.install()`, `python -m mip install`) |
| `argparse` | micropython-lib | A small `argparse`, for `mip`'s command line and your own scripts |

A module of the same name in `lib/` does not replace a frozen one, because
`.frozen` comes first on `sys.path`. Change `MICROPYPATH` to put `lib/`
first ([general.md](general.md)).

## Not in this build

| Module | Why |
|---|---|
| `_thread`, `threading` | Threads are switched off: the badge's Linux has no MMU. Use `asyncio` |
| `ffi`, and micropython-lib's `unix-ffi` packages (such as `sqlite3`) | `ffi` needs libffi and shared libraries |
| `btree` | Switched off at build |
| `network` | Wi-Fi is WhyOS's; sockets work as soon as the badge has a network |
| `bluetooth` | The Unix port can only reach a Bluetooth controller over a UART, with a thread |
| `machine.Pin`, `I2C`, `SPI`, `UART`, `ADC`, `PWM`, `RTC`, `WDT` ... | The ESP32 port's; on Linux the drivers own the hardware ([hardware.md](hardware.md)) |
| `esp`, `esp32`, `neopixel`, `espnow`, `onewire` | ESP32 port only |
| `marshal`, `weakref` | Not at this build level |
| `subprocess`, `os.fork`, `os.uname` | Not in the Unix port; `os.system()` runs a command |

micropython-lib's pure-Python packages (`python-stdlib`: `base64`,
`datetime`, `logging`, `os-path`, `string`, `itertools` and more) install
with `mip`; only `base64` has been tried (simulator). A package that needs
one of the modules above does not work.

## Notes per module

### os

The Unix port's `os`, on Linux's files:

- Files and directories: `listdir`, `ilistdir`, `mkdir`, `rmdir`,
  `remove`/`unlink`, `rename`, `stat`, `statvfs`, `chdir`, `getcwd`, `sync`,
  `sep`.
- Environment and processes: `getenv`, `putenv`, `unsetenv`, `system`.
- `urandom(n)`: random bytes from the kernel.
- `mount`, `umount`: with `vfs.VfsPosix` only.
- `errno()`: the last C error number.
- Not there: `uname` (use `platform.platform()`), `dupterm`, `os.path`
  (`mip install os-path` adds micropython-lib's pure-Python one; not
  tested).

### time

- `time()` and `time_ns()`: the clock, in seconds or nanoseconds since
  1970. Right only after NTP ([general.md](general.md)).
- `localtime()`, `gmtime()`, `mktime()`: tuples as in MicroPython (year,
  month, day, hour, minute, second, weekday, yearday).
- `sleep(s)` (also fractions), `sleep_ms()`, `sleep_us()`.
- `ticks_ms()`, `ticks_us()`, `ticks_cpu()`, `ticks_add()`, `ticks_diff()`:
  for measuring time; they do not jump when the clock is set.
- `clock()`: processor time of the program.

### socket

The Unix port's `socket`, on Linux's sockets:

- `socket(family, type, proto)`, `getaddrinfo`, `inet_pton`, `inet_ntop`.
- Families and types: `AF_INET`, `AF_INET6` (the constant is there; the
  badge's kernel has no IPv6), `AF_UNIX`, `SOCK_STREAM`, `SOCK_DGRAM`,
  `SOCK_RAW`.
- Options: `SOL_SOCKET` with `SO_REUSEADDR`, `SO_BROADCAST`,
  `SO_KEEPALIVE`, `SO_ERROR`, `SO_LINGER`; `IP_ADD_MEMBERSHIP` for
  multicast. Other levels, such as `IPPROTO_TCP` with `TCP_NODELAY`, have
  no names here: pass the numbers.
- Methods: `connect`, `bind`, `listen`, `accept`, `send`, `sendall`,
  `sendto`, `recv`, `recvfrom`, `read`, `readinto`, `readline`, `write`,
  `setsockopt`, `setblocking`, `settimeout`, `makefile`, `fileno`, `close`.
- **An address can be raw bytes.** `bind` and `connect` take any object
  with the bytes of a C `struct sockaddr`, which is what `getaddrinfo`
  returns. That reaches socket types the module has no helper for, such
  as a Unix socket. Derived from the source; not tested on the badge:

```python
import socket, struct
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(struct.pack("<H", socket.AF_UNIX) + b"/var/run/example.sock\0")
```

  `socket.sockaddr(addr)` turns such bytes back into a tuple.

Tested in the simulator: a TCP server and client on the badge itself, and
HTTPS to micropython.org.

### select

`select.poll()` and `select.ipoll()` with `register`, `unregister`,
`modify` and `poll`, and `POLLIN`, `POLLOUT`, `POLLERR`, `POLLHUP`. They
take sockets, files and streams, and also a plain file descriptor (an
integer), so a script can wait on a device file opened with `open()`.
`select.select()` is not there.

### ssl and tls

- `ssl.wrap_socket(sock, server_hostname=...)`, `ssl.SSLContext` and the
  lower `tls` module, on Mbed TLS 3.6.6 (`tls.MBEDTLS_VERSION`); client
  and server.
- **No certificate check by default.** `ssl.wrap_socket()` defaults to
  `CERT_NONE`, and `ssl.SSLContext` sets `CERT_NONE` too. That holds for
  `requests` and `mip`, so a man in the middle can change what they
  download. To check a server, load its CA and require it:

```python
import ssl, socket
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.load_verify_locations(cafile="/mnt/sd/badge/appdata/micropython/ca.pem")
ctx.verify_mode = ssl.CERT_REQUIRED
s = socket.socket()
s.connect(socket.getaddrinfo("example.com", 443)[0][-1])
s = ctx.wrap_socket(s, server_hostname="example.com")
```

- The badge has no system bundle of CA certificates (Buildroot's
  `ca-certificates` is not in WhyOS; the store and the browsers carry their
  own). Bring the CA you need as a file. Not tested on the badge.
- Certificate checks need the right time: before NTP has set the clock,
  a valid certificate can look expired.

### machine

Most of `machine` belongs to a microcontroller and is missing on Linux.
What is there:

- `mem8`, `mem16`, `mem32`: read and write memory by address. In the Unix
  port, addresses go through `/dev/mem`. On the badge's NOMMU Linux this is
  not tested and can hang the badge; reach hardware through its Linux
  driver instead ([hardware.md](hardware.md)).
- `time_pulse_us`, `idle`, `PinBase`, `Signal`: there for compatibility;
  without a `Pin` class they have little use.

### framebuf

`framebuf.FrameBuffer(buf, w, h, framebuf.RGB565)` draws pixels, lines,
rectangles, ellipses, polygons and 8 × 8 text into a `bytearray`. The
badge's screen is also RGB565 (720 × 720), so a buffer can be written to
`/dev/fb0` ([hardware.md](hardware.md)); the font is MicroPython's own
8 × 8 one, not WhyOS's.
