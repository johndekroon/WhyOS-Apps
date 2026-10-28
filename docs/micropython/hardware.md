# The badge's hardware from Python

Status: 2026-10-28. **Nothing on this page has been tried from MicroPython
yet.** It lists the interfaces WhyOS offers to any Linux program and what
this MicroPython build can do with them, derived from the build and from
the why2025-linux documents named in each row.

## Why not `machine.Pin`

On an ESP32 board MicroPython owns the chip. On the badge, Linux owns it:
its drivers handle the screen, the keyboard, the sensors, USB and the
ESP32-C6 that does Wi-Fi, Bluetooth and LoRa. A Python script talks to
those drivers through files in `/dev` and `/sys`, with `open()`, `read()`,
`write()`, `select.poll()` and `socket`, like any Linux program.

One gap matters: **this build has no `ioctl()`** (no `fcntl` module, and
`ffi` is left out). Device files that need an `ioctl` call are out of
reach from Python, such as `/dev/gpiochip0` (GPIO lines) and `/dev/i2c-*`.
Files that work with `read` and `write` alone are fine.

## What can be reached

| Hardware | Interface | From this MicroPython | More in |
|---|---|---|---|
| Sensors: BME680 (temperature, humidity, pressure, gas), BMI270 (motion) | `/sys/bus/iio/devices/iio:device*/` files: one value per file | Read the files | `why2025-linux/README.md`, `sdapps/README.md` (sensor panel) |
| The four RGB LEDs of the frontpanel addon (not on the badge itself) | `/sys/class/leds/rgb:indicator-{0..3}/` (`brightness`, `multi_intensity`, `trigger`) | Write the files | `why2025-linux/HARDWARE.md`, LEDs |
| A computer on the bottom port (J8): keyboard and mouse | `/dev/hidg0` (8-byte boot keyboard reports), `/dev/hidg1` (4-byte mouse reports: buttons, X, Y, wheel); `/var/run/usb.state` says `computer=1` when one is connected | Write the reports with `open(..., "wb")`; HIDeous uses the same files | `why2025-linux/docs/USB.md` |
| A USB serial device on J8 (CH340, CP210x, FTDI, CDC-ACM, a Bus Pirate) | `/dev/ttyUSB*`, `/dev/ttyACM*` | `open()` plus `termios.tcsetattr`/`setraw` for the speed and raw mode | `why2025-linux/docs/USB.md` |
| MeshCore messages over LoRa | `meshcored`'s Unix socket `/var/run/meshcored.sock`: MeshCore's companion protocol, frames `<` + length (2 bytes, little-endian) to the daemon, `>` + length back | `socket.AF_UNIX` with a raw address ([library.md](library.md), socket). **One client at a time:** the newest connection wins, so the MeshCore app loses its connection while a script is connected, and the other way round | `why2025-linux/docs/LORA.md`, meshcored |
| The LoRa radio itself | `/dev/lora0`: one command per `write()`, one event per `read()`, `poll()` works; protocol in `why2025-linux/lora/esp_lora_proto.h` | `open()`, `struct`, `select.poll`. Only while LoRa is off in Settings: `meshcored` holds the device otherwise, and there is one opener at a time | `why2025-linux/docs/LORA.md` |
| Bluetooth LE | A raw HCI socket: `AF_BLUETOOTH` (31), `SOCK_RAW`, `BTPROTO_HCI` (1), bound to `hci0` with `HCI_CHANNEL_USER`; HCI commands and events as bytes | Write `1` to `/sys/module/esp32_spi/parameters/bt_enable` first, then `socket.socket(31, socket.SOCK_RAW, 1)` and `bind()` with a raw address. Exclusive: not while `ble_scan`, `ble_adv` or the menu's beacon runs | `why2025-linux/patches/buildroot/overlay-src/ble_scan.c`, `docs/KNOWN-ISSUES.md` |
| The screen, 720 × 720 RGB565 | `/dev/fb0`, written with `seek` and `write` (no mmap on this Linux) | `framebuf` can draw into a `bytearray` in the same format. From the menu appterm owns the screen, so this only suits a script that is the only thing drawing | `why2025-linux/docs/APPS.md`, writing an app |
| The keypad | `/dev/input/event0`, evdev events | Read and unpack them with `struct`. Do not grab the device: `apprun` reads it too, for the logo key | `why2025-linux/docs/APPS.md` |

## Before you transmit

- **LoRa:** the radio can be damaged for good if it transmits without a
  matching antenna. WhyOS asks about the antenna when LoRa is switched on
  in Settings; a script that opens `/dev/lora0` itself skips that question
  and has to make sure on its own. On 868 MHz the transmit power and duty
  cycle limits apply to the script too.
- **LoRa heartbeat:** the C6 switches the radio off when it has heard no
  command for 30 s (`LORA_HEARTBEAT_MS`). A script that holds
  `/dev/lora0` must send a command, such as `STATUS`, well within that
  time, also while it only listens. `meshcored` does this every second.
- **Bluetooth:** the first bind after a boot can fail with `EBUSY` while
  the kernel sets up `hci0`; a second try works
  (`why2025-linux/docs/KNOWN-ISSUES.md`).
- **USB HID:** HIDeous types through the same `/dev/hidg0`. A script and
  HIDeous at the same time would mix their key presses.

## What is not reachable now

| Hardware | Why |
|---|---|
| GPIO on the headers, the I2C bus on J7 | Need `ioctl` on `/dev/gpiochip0` (OHMinous bit-bangs I2C over GPIO lines) |
| The screen's brightness, the keyboard light | WhyOS's settings `display.brightness` and `display.keys` (`why2025-linux/docs/SETTINGS.md`); whether a script can change them while the badge runs is not looked into |
| Audio, camera | Not supported by WhyOS |
| Battery level | The badge cannot measure it |
