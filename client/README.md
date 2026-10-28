# store: the WhyOS app store client

The `store` program of [docs/client.md](../docs/client.md): the command
line, and without arguments the full-screen UI on the badge. C, static, for
the badge (BFLT) and for the host (tests, CI).

| File | What |
|---|---|
| `src/cli.c` | the commands (`store update`, `install`, `remove`, ...) |
| `src/lifecycle.c` | install, update, remove, verify, repair, adopt, maintain (device.md §5); app.conf, `.package/`, command wrappers |
| `src/wpk.c` | streaming package reader with the checks of package.md §1.2 |
| `src/net.c` | HTTPS with mbedTLS: certificate checks, redirects, resume |
| `src/index.c` | the index and app details, cached on the card |
| `src/registry.c` | registry, card id, journal, pending cleanups |
| `src/paths.c`, `src/sources.c` | where things live; sources and mirrors |
| `src/json.c` | JSON reading (jsmn, `third_party/`) and writing |
| `src/ui.c` | the screens (client.md §4), drawn with why2025-linux's `libwb/` (`wb.h`, `wbstatus.h`) in the menu's style |
| `src/qoi.c` | QOI decoder for icons and screenshots |
| `roots.pem` | the CA roots it trusts, picked by `mkroots.py` from Mozilla's list |
| `deps.sh` | builds the pinned mbedTLS for the host or the badge |
| `tests/test_client.py` | the lifecycle against a local HTTPS source |
| `tests/test_ui.py` | the screens on the host: keys from a script, screens as PPM (`SHOTS=dir` keeps them as PNG) |

```
make -C client host test        # Linux; builds mbedTLS once into client/.deps
client/store-host check-package dist/v1/packages/gnuboy-1.0.0.wpk
client/build.sh                 # the badge's build/store (needs ~/br-badge)
```

The screens need why2025-linux (its `libwb/`) next to this repo (`WHY2025_LINUX`, default
`../../why2025-linux`); without it `make test` skips them. The program is
`apps/store/` in the store, installed in flash. On the badge,
`STORE_UI_TIMING=1` writes a line per frame to stderr: its time, the bands
drawn, and libwb's recording, scene, glass and writing-out times (µs). The
screens draw only the bands that changed (libwb `wb_bands`); `WB_FULL=1`
draws every band, and `tests/test_ui.py` compares the two.

For the tests and the host build, `HOME`, `STORE_SD`, `STORE_FLASH_APPS`,
`STORE_TMP`, `STORE_SYS_SOURCES` and `STORE_RELEASE` move every path into a
temporary root (`src/paths.c`).
