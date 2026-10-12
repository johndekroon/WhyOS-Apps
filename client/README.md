# store: the WhyOS app store client

The `store` program of [docs/client.md](../docs/client.md): command line
now, the full-screen UI next (phase 2). C, static, for the badge (BFLT)
and for the host (tests, CI).

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
| `roots.pem` | the CA roots it trusts, picked by `mkroots.py` from Mozilla's list |
| `deps.sh` | builds the pinned mbedTLS for the host or the badge |
| `tests/test_client.py` | the lifecycle against a local HTTPS source |

```
make -C client host test        # Linux; builds mbedTLS once into client/.deps
client/store-host check-package dist/v1/packages/gnuboy-1.0.0.wpk
```

For the tests and the host build, `HOME`, `STORE_SD`, `STORE_FLASH_APPS`,
`STORE_TMP`, `STORE_SYS_SOURCES` and `STORE_RELEASE` move every path into a
temporary root (`src/paths.c`).
