# meshcore_py for the badge's MicroPython

[meshcore_py](https://github.com/meshcore-dev/meshcore_py) (MIT), the
Python library for MeshCore companion radios, frozen into the badge's
MicroPython (`sdapps/build.sh micropython`), so a script can `import
meshcore` and talk to `meshcored`, the badge's MeshCore node
([`docs/LORA.md`](../../../docs/LORA.md)): send and receive channel and
direct messages, contacts, adverts, channels, telemetry. The API is
upstream's ([its README](https://github.com/meshcore-dev/meshcore_py#readme));
the badge adds one way to connect:

```python
import asyncio
from meshcore import MeshCore, EventType

async def main():
    mc = await MeshCore.create_unix()     # /var/run/meshcored.sock; LoRa must be on
    mc.subscribe(EventType.CONTACT_MSG_RECV, lambda e: print(e.payload["text"]))
    await mc.start_auto_message_fetching()
    await mc.commands.send_chan_msg(0, "hello from Python")
    await asyncio.sleep(60)

asyncio.run(main())
```

[`examples/ping_bot.py`](examples/ping_bot.py) is a whole bot: "ping" in
a direct message, or "!ping" on a channel, gets "pong" back. The build
puts it in the app as `examples/ping_bot.py`.

**A bot transmits.** Only with the antenna on (Docs/03-veiligheid.md
§3.5); LoRa is switched on in Settings, which asks about the antenna.

## What is what

| | |
|---|---|
| `upstream/` | meshcore_py 2.3.17 (`src/meshcore`) and pycayennelpp 2.4.0, which it imports for sensor data; unmodified, MIT, with their licences. `VENDORED` has the commits; `vendor.sh` refreshes both |
| `patches/` | our two changes to upstream: `events.py` runs an async callback by calling it (0001), `set_channel()` pads without `bytes.ljust` (0002) |
| `port/meshcore/` | ours, copied over upstream's package: `__init__.py` (replaces upstream's), `_compat.py`, `_asyncio.py`, `_logging.py`, `unix_cx.py` |
| `stage.sh DIR` | `DIR/meshcore` and `DIR/cayennelpp`: upstream, patched, with `port/` on top. The build freezes that; the tests import it |
| `../0001-int-from_bytes-signed.patch` | MicroPython's `int.from_bytes()` takes `signed=` (and `byteorder=` by name); the library decodes SNR, RSSI and coordinates with it. Applies to MicroPython itself, so every script gets it |
| `examples/` | `ping_bot.py` |
| `test/` | the host tests, below |

## How it runs on MicroPython

MicroPython 1.29 lacks parts of CPython the library uses. `__init__.py`
puts `_compat` in `sys.modules` as `enum`, `typing`, `dataclasses`,
`inspect`, `json`, `traceback`, `struct`, `secrets`, `Crypto` and
`serial_asyncio_fast` while the library's modules are imported, then
takes the names out again (`secrets` stays: a method imports it later),
so a script's own `import enum` still fails as before.

- **enum:** no metaclasses in MicroPython, so a class body cannot make
  members. `__init__.py` turns the attributes of each `Enum` class in
  `packets` and `events` into members (`.name`, `.value`,
  `EventType("contact_message")`) before the other modules import them.
  Iterating over an enum class (`for t in EventType`) does not work.
- **asyncio:** `Future`, `Queue`, `wait()`, `wait_for()` on a Future,
  `get_running_loop()` with `create_future()`, `create_connection()`
  (asyncio protocols over TCP) and `create_unix_connection()`,
  `iscoroutinefunction()`. Added to the `asyncio` module, where they stay
  for scripts too. `wait()` hooks into a task the way MicroPython's
  `gather()` does, not by awaiting it: in MicroPython, cancelling a task
  that awaits another cancels that one too.
- **Callbacks:** MicroPython cannot tell an async function from a closure
  or bound method of one, so the dispatcher calls every callback and runs
  what it returns if that is a coroutine (patch 0001). Plain callbacks
  still run at once, as upstream.
- **Tasks** have no `add_done_callback()`: the library's background tasks
  remove themselves from its set when they end.
- **struct** with spaces in a format (`'<h b b I I'`), `json.dumps(default=)`,
  AES-ECB, SHA-256 and HMAC-SHA256 (`cryptolib`, `hashlib`) for
  pycryptodome, `traceback.format_exc()`.
- **logging:** micropython-lib's takes no `exc_info=`, which the library
  passes; `_logging.py` is a small one that does. It stays as `logging`,
  so `logging.basicConfig(level=logging.DEBUG)` after `import meshcore`
  shows the library's messages.
- `print(event)` shows `Event(type=..., payload=..., attributes=...)`, as
  the dataclass does in CPython.

## Limits

- **meshcored serves one app at a time.** A new connection replaces the
  old one ([`docs/LORA.md`](../../../docs/LORA.md)): opening the MeshCore
  app disconnects a bot (`EventType.DISCONNECTED`), and the app connects
  again every 2 s while it is open. A bot with `auto_reconnect=True` and
  the app then take turns. Messages that arrive go to whichever of them
  fetches them.
- **One command at a time per connection.** The library matches a reply
  to a command by its type only (upstream's design, the same in CPython):
  two commands waiting for, say, `OK` at once can take each other's
  reply. A bot handles messages one after the other (the example uses an
  `asyncio.Queue`).
- **No BLE or serial companion.** `create_ble()` needs bleak,
  `create_serial()` pyserial; neither is there. `create_tcp()` works (a
  node elsewhere, or meshcored with `-t PORT`).
- **Answer a channel message after a pause.** A LoRa radio that sends
  hears nothing: a reply sent at once can collide with repeaters passing
  the message on. On the simulated ether, an immediate reply on a channel
  was lost every time and one after 2 s never (2026-10-28).

## Cost

MicroPython grows from 723 KB to 837 KB. On the badge (2026-10-28):
`import meshcore` takes about 100 ms and 42 KB of the 2 MB heap; a
session with contacts and a minute of listening used 52 KB.

## Tests

Host, any Linux, with a MicroPython built for it (patch 0001 applied) and
the host meshcored (`make -C meshcore`):

```bash
python3 test/test_meshcore_py.py MICROPYTHON ../../../meshcore/build-host/meshcored [CPYTHON]
```

1. `check.py`, as a bot on the nodes' Unix sockets: `create_unix`,
   `self_info`, device query, clock, a channel message with emoji echoed
   back, adverts both ways into contacts, a direct message with its ack
   and a reply, an attribute filter and unsubscribe, a command timeout,
   disconnect (9 checks).
2. `examples/ping_bot.py` on Alice; `bot_peer.py` on Bob gets its pongs to
   a direct message and on the Public channel.
3. `sweep.py`: 40 commands over TCP (settings, coordinates north and
   south, channels by hashtag and by key, telemetry as Cayenne LPP,
   statistics, signing, contacts, an exported contact, a direct and a
   channel message, a channel message decrypted from the RX log), under
   MicroPython and, given `CPYTHON` (a Python with `meshcore==2.3.17` from
   PyPI), under CPython on copies of the same two nodes. The results must
   be the same line for line, apart from clocks, counters and radio noise.

Passed 2026-10-28 in the build VM, three runs in a row. Upstream's
`send_msg_with_retry()` with the flood fallback: 40 messages under both
interpreters, all delivered and acked in 110 s each. Once, in about thirty
runs of `check.py`, a `reset_path` and the retry after it got no reply
within the timeout; not seen again in twelve runs with debug logging.

Badge: `Tools/usb/hil/test_hil_meshcore_py.py` checks `self_info` against
the badge's settings (name, radio, power), the model, the clock, contacts
and the Public channel, and the cost of the import; it sends nothing over
LoRa. Passed 2026-10-28 with this build placed by hand in
`/mnt/sd/badge/apps/micropython`.

## Updating

Set the refs in `vendor.sh`, run it, run the host tests. A patch that no
longer applies, or a new CPython feature, shows up there; the sweep
against CPython (install the same version from PyPI) finds parsing that
differs.
