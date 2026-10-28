# Using the badge from Python

MicroPython on the badge has no `machine.Pin` or `I2C`: Linux drivers own
the hardware. A Python program reaches it the way any Linux program does,
by reading and writing files in `/sys` and `/dev`. This page shows how for
each part of the badge.

## Sensors

The badge has two sensors: a BME680 (temperature, humidity, air pressure)
and a BMI270 (motion). Each value is a small text file in a folder under
`/sys/bus/iio/devices/`. Look the folder up by the sensor's name, because
the numbering can differ:

```python
import os

def sensor(name):
    base = "/sys/bus/iio/devices/"
    for d in os.listdir(base):
        try:
            with open(base + d + "/name") as f:
                if f.read().strip() == name:
                    return base + d + "/"
        except OSError:
            pass
    raise OSError("no sensor " + name)

def value(path):
    with open(path) as f:
        return f.read().strip()

env = sensor("bme680")
imu = sensor("bmi270")

temperature = int(value(env + "in_temp_input")) / 1000          # °C
humidity = float(value(env + "in_humidityrelative_input"))      # %
print("%.1f °C, %.0f %%" % (temperature, humidity))

scale = float(value(imu + "in_accel_scale"))                    # m/s² per step
ax = int(value(imu + "in_accel_x_raw")) * scale
ay = int(value(imu + "in_accel_y_raw")) * scale
az = int(value(imu + "in_accel_z_raw")) * scale
print("acceleration %.2f %.2f %.2f m/s²" % (ax, ay, az))
```

- Read the file again for every new measurement; the value is measured
  when the file is read.
- Lying flat, the acceleration of gravity (about 9.8 m/s²) shows on one
  axis; tilting the badge moves it to the others. That makes a tilt
  control for a game.
- `in_pressure_input` gives the air pressure in kPa, but this badge's
  sensor reads about 2.3 times too high (around 225–250 instead of about
  100), a known calibration mismatch (`why2025-linux/sdapps/sensorpanel`).
- The Sensor panel app in the store shows the same values, handy to
  compare.

## LEDs

The four RGB LEDs sit on the **frontpanel addon**, not on the badge
itself; without the addon nothing lights up. Each LED is a folder
`/sys/class/leds/rgb:indicator-0/` to `-3/`:

```python
LED = "/sys/class/leds/rgb:indicator-%d/"

def led(n, r, g, b, brightness=255):
    with open(LED % n + "multi_intensity", "w") as f:
        f.write("%d %d %d" % (r, g, b))
    with open(LED % n + "brightness", "w") as f:
        f.write(str(brightness))

with open(LED % 0 + "multi_index") as f:
    print("colour order:", f.read().strip())    # the order multi_intensity uses

led(0, 255, 0, 0)       # red
led(1, 0, 0, 255, 40)   # dim blue
```

`brightness` 0 switches an LED off. The file `trigger` can make the kernel
blink it by itself (`timer`, `heartbeat`).

## The internet

The badge connects to Wi-Fi in Settings > Connection > Wi-Fi; Python then
simply uses the network. There is no `network` module and nothing to set
up in the program.

Fetching a web page or a JSON API with `requests`:

```python
import requests

r = requests.get("http://ip-api.com/json/")
info = r.json()
r.close()
print("The badge is in", info["city"], info["country"])
```

`requests` also has `post`, `put`, `delete` and `head`, and takes
`headers=` and `json=`. Close every response with `r.close()`.

**HTTPS works, but does not check certificates by default.** That suits
trying things out; for anything that matters, check the server against the
certificate of the authority that signed it (a PEM file you put on the
card; the badge has no built-in list of them):

```python
import ssl, socket

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.load_verify_locations(cafile="/mnt/sd/badge/appdata/micropython/ca.pem")
ctx.verify_mode = ssl.CERT_REQUIRED
s = socket.socket()
s.connect(socket.getaddrinfo("example.org", 443, socket.AF_INET)[0][-1])
s = ctx.wrap_socket(s, server_hostname="example.org")
s.write(b"GET / HTTP/1.0\r\nHost: example.org\r\n\r\n")
print(s.read(200))
s.close()
```

A small web server on the badge, reachable from your computer at
`http://BADGE:8080/`:

```python
import socket

s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(socket.getaddrinfo("0.0.0.0", 8080)[0][-1])
s.listen(2)
while True:
    c, addr = s.accept()
    c.recv(1024)
    c.send(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\nHello from the badge\n")
    c.close()
```

UDP works the same way with `socket.SOCK_DGRAM`.

**The badge has no IPv6.** Ask `socket.getaddrinfo()` for IPv4 addresses
with `socket.AF_INET`, as in the example above: many sites also have an
IPv6 address, and connecting to it fails with `OSError: 97`. MicroPython
1.29.0 build 1 (the one in the store on 2026-10-28) returns those IPv6
addresses first, so there `requests` fails for such sites too (Google,
example.org); build 2 returns only IPv4 addresses, as Python on a PC does.

Without a network, `socket.getaddrinfo()` and `requests` raise `OSError`;
catch it to show a message instead of a traceback. A certificate that does
not match the CA file raises `ValueError` in `wrap_socket()`.

## Time

- `time.time()` and `time.localtime()` give the clock. The badge has no
  battery-backed clock: it sets the time over the internet shortly after
  Wi-Fi connects. Until then the time is wrong; `time.localtime()[0] < 2026`
  tells you so.
- `time.localtime()` uses the badge's time zone (Settings > General > Time
  zone); `time.gmtime()` gives UTC.
- For measuring durations use `time.ticks_ms()` and
  `time.ticks_diff(new, old)`; they are not affected when the clock is set.

```python
import time
y, mo, d, h, mi, s, wd, yd, dst = time.localtime()   # nine values, as in CPython
print("%02d:%02d" % (h, mi))
```

### Calendars (.ics links)

From MicroPython 1.29.0 build 2 on, the `ics` module reads calendars in
iCalendar format: the "iCal" or "webcal" link that Google Calendar,
Outlook, Nextcloud and event sites give. The next 90 days of a calendar:

```python
import ics, time

URL = "https://calendar.google.com/calendar/ical/nl.dutch%23holiday%40group.v.calendar.google.com/public/basic.ics"

now = time.time()
cal = ics.get(URL, since=now)                 # since: leave out what is over
print(cal.name, len(cal.events), "events")
for e in cal.between(now, now + 90 * 86400):  # the next 90 days
    y, mo, d, h, mi = time.localtime(e.start)[:5]
    if e.all_day:
        print("%02d-%02d       %s" % (d, mo, e.summary))
    else:
        print("%02d-%02d %02d:%02d %s" % (d, mo, h, mi, e.summary))
```

- `ics.get(url)` takes `http://`, `https://` and `webcal://` links;
  `ics.load(path)` reads a file, `ics.parse(text)` a string.
- `cal.between(start, end)` gives the events in that time, by start; an
  event that repeats appears once for each time. `cal.events` has them
  as in the file, without the repeats worked out.
- Each event has `summary`, `location`, `description`, `start`, `end`,
  `all_day`, `uid`, `status`, `url`, `rrule` and `recurrence_id`. Times
  are seconds since 1970, like `time.time()`; `time.localtime()` shows
  them in the badge's time zone. An all-day event starts at midnight.
- **Repeats** (RRULE) work out daily, weekly, monthly and yearly rules
  with their usual parts (every 2nd week, the last Friday of the month,
  until a date, so many times), plus extra and skipped dates and single
  times that were moved or cancelled. Rare rule parts (BYWEEKNO,
  BYYEARDAY, hourly rules) give only the first time.
- **Time zones** come from the calendar itself, which usually describes
  the ones it uses, or else from the badge's list of 77 zones.
- **Memory:** each event kept takes about half a KB. With `since=` the
  events that are over are left out while reading; Google's Dutch
  holidays (200 events) then keep 51 KB instead of 107 KB. A personal
  calendar with years of history needs `since=`.
- `ics` on PyPI is something else and does not run on MicroPython; `mip`
  does not install from PyPI.

## XML (feeds, schedules, GPX)

From MicroPython 1.29.0 build 2 on, `xml.etree.ElementTree` reads and
writes XML as Python on a computer does: RSS and Atom feeds, GPX tracks,
event schedules, the answers of many web APIs. A small document at once:

```python
import xml.etree.ElementTree as ET

root = ET.fromstring('<feed><item id="1"><title>Hello</title></item></feed>')
for item in root.findall("item"):
    print(item.get("id"), item.findtext("title"))
```

A big one piece by piece, while it downloads: the talks in hall One on the
second day of the 39th Chaos Communication Congress (1.9 MB, 1008 talks):

```python
import requests
import xml.etree.ElementTree as ET

URL = "https://fahrplan.events.ccc.de/congress/2025/fahrplan/schedules/schedule.xml"

r = requests.get(URL)
for event, el in ET.iterparse(r.raw):      # each element once it is complete
    if el.tag == "event":
        if el.findtext("room") == "One" and el.findtext("date").startswith("2025-12-28"):
            print(el.findtext("start"), el.findtext("title"))
        el.clear()                           # done with it: free what is in it
r.close()
```

- `ET.parse(path)` and `ET.fromstring(text)` give the whole tree. `find`,
  `findall`, `findtext`, `iter`, `get`, `.text` and `.attrib` work as in
  CPython, with the same paths (`channel/item[@id='2']/title`,
  `.//title`). Namespaces become `{uri}tag`; with
  `namespaces={"a": "http://www.w3.org/2005/Atom"}` you can write
  `a:entry`.
- **Memory:** a whole tree takes about 2.4 times the size of the file, so
  with the usual 2 MB files up to about 500 KB fit (that one takes 3.3 s).
  For bigger ones use `ET.iterparse()` as above: it reads in small pieces
  and gives each element when its end tag has come; `el.clear()` there
  keeps memory low. The congress schedule takes 16 s over Wi-Fi and
  190 KB at most.
- Writing: `ET.tostring(root, encoding="unicode")` gives the text,
  `ET.ElementTree(root).write(path)` a file, `ET.indent(root)` adds line
  breaks and indentation first. `ET.Element`, `ET.SubElement` and
  `el.set()` build a tree.
- Encodings: UTF-8, ASCII, ISO-8859-1 and Windows-1252, as the file
  declares. Not there: UTF-16, entities declared in a DOCTYPE,
  `xml.dom.minidom`, `xml.sax` and `lxml`.
- One difference from CPython: `el.attrib` is an `OrderedDict`, so the
  attributes keep their order, and `==` then also compares the order. To
  compare with a dict, write `dict(el.attrib) == {"id": "1"}`.

## Typing on a computer (USB keyboard and mouse)

Connected to a computer by its **bottom** USB port (with the side port
unplugged), the badge is a USB keyboard and mouse for that computer, as in
the HIDeous app (WhyBadge documentation, chapter 8.6). A program can type
by writing keyboard reports to `/dev/hidg0`: 8 bytes, the modifier keys
in the first, the key in the third.

```python
import time

SHIFT = 0x02
KEYS = {" ": 44, "\n": 40, "\t": 43}
for i, ch in enumerate("abcdefghijklmnopqrstuvwxyz"):
    KEYS[ch] = 4 + i
for i, ch in enumerate("1234567890"):
    KEYS[ch] = 30 + i

def type_text(text):
    with open("/dev/hidg0", "wb") as kb:
        for ch in text:
            mod = SHIFT if ch.isupper() else 0
            code = KEYS.get(ch.lower())
            if code is None:
                continue                                    # not in this small table
            kb.write(bytes([mod, 0, code, 0, 0, 0, 0, 0]))  # press
            kb.write(bytes(8))                              # release
            time.sleep_ms(5)

type_text("Hello from the badge\n")
```

- The computer turns key codes into characters with **its own** keyboard
  layout. This table matches the US layout; on a German or French layout
  some letters come out differently.
- Is a computer connected? `/var/run/usb.state` contains `computer=1`.
  Without one, writing to `/dev/hidg0` raises `OSError: 108`.
- The mouse is `/dev/hidg1`, 4 bytes: buttons (1 left, 2 right, 4 middle),
  X, Y and wheel as signed steps (`struct.pack("<Bbbb", 0, 10, 0, 0)` moves
  10 to the right).
- HIDeous uses the same files; do not run both at once.

## Serial devices

A USB serial adapter or a device such as a Bus Pirate on the bottom port
appears as `/dev/ttyUSB0` or `/dev/ttyACM0`. `termios` sets the speed:

```python
import termios, select

f = open("/dev/ttyACM0", "r+b")
fd = f.fileno()
termios.setraw(fd)
attrs = termios.tcgetattr(fd)
attrs[4] = attrs[5] = termios.B115200            # input and output speed
termios.tcsetattr(fd, termios.TCSANOW, attrs)

p = select.poll()
p.register(fd, select.POLLIN)

def read_reply(wait_ms=500):
    data = b""
    while p.poll(wait_ms):
        data += f.read(1)
        wait_ms = 50                             # stop when the device goes quiet
    return data

f.write(b"i\r\n")
print(read_reply())
f.close()
```

The speeds with a name in this build are `B9600`, `B57600`, `B115200`,
`B230400`, `B460800`, `B500000`, `B576000`, `B921600`, `B1000000` and
`B1152000`.

## LoRa: MeshCore messages and bots

The badge is a [MeshCore](https://meshcore.co.uk/) node: `meshcored`
runs the LoRa radio, and the MeshCore app is one way to use it. Python is
another. The `meshcore` module is built into MicroPython, so a script can
read and send messages in channels and direct messages, see contacts and
adverts, and make a bot that answers. It is
[meshcore_py](https://github.com/meshcore-dev/meshcore_py), the library
the MeshCore tools on a computer use, with one addition for the badge:
`MeshCore.create_unix()`, which connects to the badge's own node.

`meshcore` comes with MicroPython 1.29.0 build 2 and later. On 2026-10-28
that version is built but not yet in the store; with an older one,
`import meshcore` raises `ImportError`.

**Before you start:**

- Screw the antenna on. The LoRa radio can be damaged for good when it
  transmits without one, and a bot transmits.
- Switch LoRa on in Settings > Connection > LoRa. Without it there is no
  node to connect to: `create_unix()` raises `OSError`.
- Close the MeshCore app. The node serves one program at a time: when the
  app opens, your script loses its connection, and the other way round.

### Reading messages

```python
import asyncio
from meshcore import MeshCore, EventType

def show(event):
    m = event.payload
    if event.type == EventType.CHANNEL_MSG_RECV:
        print("channel", m["channel_idx"], m["text"])   # text is "name: message"
    else:
        print("from", m["pubkey_prefix"], m["text"])

async def main():
    mc = await MeshCore.create_unix()
    print("I am", mc.self_info["name"])
    mc.subscribe(EventType.CHANNEL_MSG_RECV, show)
    mc.subscribe(EventType.CONTACT_MSG_RECV, show)
    await mc.start_auto_message_fetching()   # also fetches what waited
    while True:
        await asyncio.sleep(60)

asyncio.run(main())
```

Messages that arrived while no program was connected wait in the node and
come in when the script starts fetching. A callback can be a plain
function or an `async def`.

### Sending

Inside `main()`, after `create_unix()`:

```python
    # on a channel: 0 is Public
    await mc.commands.send_chan_msg(0, "hello from Python")

    # a direct message, to a contact by name
    await mc.ensure_contacts()
    friend = mc.get_contact_by_name("Mend0x")
    if friend:
        r = await mc.commands.send_msg_with_retry(friend, "hi!")
        print("delivered" if r else "no ack")
```

`send_msg_with_retry()` waits for the other node's acknowledgement and
tries again (the last time by flooding the whole mesh); it returns `None`
when no ack came. `send_msg()` sends once and does not wait.

Every command returns an event: `r.type` says what came back
(`EventType.OK`, `EventType.ERROR`, ...) and `r.payload` the details.
`print(r)` shows both.

### What else the library does

| | |
|---|---|
| `mc.contacts`, `mc.get_contact_by_name()`, `mc.get_contact_by_key_prefix()` | The node's contacts (after `await mc.ensure_contacts()`) |
| `mc.commands.send_advert(flood=True)` | Tell the mesh this node exists |
| `mc.commands.get_channel(1)`, `mc.commands.set_channel(1, "#badgebots")` | Channels: a name with `#` is a hashtag channel, whose key follows from the name, so everyone who types it is in |
| `mc.commands.set_name()`, `set_coords()`, `get_time()`, `get_self_telemetry()` | The node itself |
| `EventType.ADVERTISEMENT`, `ACK`, `PATH_UPDATE`, `RX_LOG_DATA` | Adverts heard, acks, routes, every packet received |

The whole API is in meshcore_py's
[README](https://github.com/meshcore-dev/meshcore_py#readme). Bluetooth
and serial connections (`create_ble()`, `create_serial()`) are not
available on the badge; `create_tcp(host, port)` connects to a node
elsewhere on the network.

### A bot

`/mnt/sd/badge/apps/micropython/examples/ping_bot.py` comes with
MicroPython: it answers "ping" in a direct message, and "!ping" on a
channel, with "pong" and how the message came in. Copy it to your scripts
folder to make it your own. It shows three rules for bots:

- **One command at a time.** The library tells the replies to commands
  apart by their type only, so two commands at once can take each other's
  reply. Put incoming messages in an `asyncio.Queue` and handle them one
  after the other, as the example does.
- **Wait a moment before answering on a channel.** A radio that is sending
  hears nothing, and repeaters pass a channel message on right after it
  arrives. The example waits 2 s; an answer sent at once was lost every
  time in our tests.
- **The node serves one program.** When the MeshCore app opens, the bot
  gets `EventType.DISCONNECTED`. `MeshCore.create_unix(auto_reconnect=True)`
  makes it connect again, which takes the node back from the app.

Keep a bot quiet: it shares the airtime of everyone on the mesh, and in
Europe a radio may only send a small part of the time.

The examples in this section ran on 2026-10-28 against two MeshCore nodes
on a simulated radio (reading, a channel message, a direct message with
its ack, losing the node to another program and taking it back). On the
badge itself the library has read the node's settings, contacts and
channels; a bot talking to another badge over the air has not been tried.

### Seeing what the library does

```python
import meshcore, logging
logging.basicConfig(level=logging.DEBUG)
```

`import logging` works after `import meshcore` (the library brings a
small `logging` of its own); `logging.DEBUG` shows every frame to and from
the node.

## Bluetooth

There is no Python library for Bluetooth. Settings has a Bluetooth beacon.

## What Python cannot reach

| Part | Why |
|---|---|
| The screen as graphics | Only the text window, see [programming.md](programming.md) |
| GPIO pins and the I2C bus on the headers | They need an `ioctl()` call, which this MicroPython does not have. The OHMinous app works with them |
| Screen brightness, the keyboard light | Set in Settings |
| Battery level | The badge cannot measure it |
| Sound | The badge has no sound under WhyOS |
