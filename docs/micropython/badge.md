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
s.connect(socket.getaddrinfo("example.org", 443)[0][-1])
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

UDP works the same way with `socket.SOCK_DGRAM`. The badge has no IPv6.

Without a network, `socket.getaddrinfo()` and `requests` raise `OSError`;
catch it to show a message instead of a traceback.

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
y, mo, d, h, mi, s, wd, yd = time.localtime()
print("%02d:%02d" % (h, mi))
```

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

The speeds with a name in this build are `B9600`, `B57600`, `B115200` and
`B230400`.

## LoRa and Bluetooth

There is no Python library for these yet. The MeshCore app sends and
receives LoRa messages; Settings has a Bluetooth beacon. Programs that talk
to the radio themselves are possible but advanced: see why2025-linux
`docs/LORA.md` before you transmit anything, because the LoRa radio can be
damaged for good when it transmits without an antenna.

## What Python cannot reach

| Part | Why |
|---|---|
| The screen as graphics | Only the text window, see [programming.md](programming.md) |
| GPIO pins and the I2C bus on the headers | They need an `ioctl()` call, which this MicroPython does not have. The OHMinous app works with them |
| Screen brightness, the keyboard light | Set in Settings |
| Battery level | The badge cannot measure it |
| Sound | The badge has no sound under WhyOS |
