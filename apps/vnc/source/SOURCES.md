# Corresponding source: VNC server

The VNC server is our own program, written for the badge. The files are
copied from WhyOS `sdapps/vnc/`:

| File | What |
|---|---|
| `vncd.c` | the server: RFB 3.8 with VNC authentication, Raw and Hextile, the keys |
| `info` | the app's screen: whether it runs, where to connect, the password |
| `test_vnc.py` | its tests: a viewer in Python against a host or RV32 build |
| `app.conf` | the menu entry of the hand-installed app (the store writes its own) |

Build: `sdapps-build.sh vnc` ([toolchain/](../../../toolchain/README.md)),
with these files in `sdapps/vnc/`, as in WhyOS; it links mbed TLS's DES
(`libmbedcrypto`). On a Linux host, `python3 test_vnc.py` builds and tests it.
