# Corresponding source: VNC server

The VNC server is our own program, written for the badge. The files are
copied from WhyOS `sdapps/vnc/`:

| File | What |
|---|---|
| `vncd.c` | the server: RFB 3.8 with VNC authentication, Raw, Hextile and ZRLE, the keys |
| `test_vnc.py` | its tests: a viewer in Python against a host or RV32 build |
| `app.conf` | the service's entry when installed by hand (the store writes its own) |

Build: `sdapps-build.sh vnc` ([toolchain/](../../../toolchain/README.md)),
with these files in `sdapps/vnc/`, as in WhyOS; it links mbed TLS's DES
(`libmbedcrypto`) and zlib. On a Linux host, `python3 test_vnc.py` builds and tests it.
