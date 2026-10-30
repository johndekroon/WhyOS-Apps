# Corresponding source: Proxmark3

Together with the upstream repository below, this directory is the
complete corresponding source of the `proxmark3` package
(docs/package.md §2.10). The patches are the thirteen commits of branch
`badge` of our copy of the client (`BADGE=1`, a build for the badge's
NOMMU Linux), made with `git format-patch` at commit `f7b6acc20`; the
toolchain is in [toolchain/](../../../toolchain/README.md).

| Upstream | URL | Commit |
|---|---|---|
| Proxmark3 (RRG/Iceman), release v4.21611 plus 636 commits | https://github.com/RfidResearchGroup/proxmark3 | `da509461b734a61994f8e430e3151e9084bf9718` |

| File | What |
|---|---|
| `0001-…` to `0007-…` | the `BADGE=1` build: smaller buffers, crapto1 without its 18 MB tables, no Lua, Python, Qt, Bluetooth, hardnested or MQTT, and the fixes found on the badge with an RDV4 (0006: the `struct stat` mismatch of uClibc-ng on riscv32, and a second and third ping on start; 0007: a line editor with history, and saves in `~/.proxmark3/dumps`) |
| `0008-…` | the app's own screens (`pm3 --ui`, `client/src/badgeui/`), drawn with libwb |
| `0009-…` to `0013-…` | fixes and additions to the screens: Sniff keeps listening (`hf 14a sniff -i`), dumps the client saves itself (iCLASS, Ultralight, …) go to Cards, a command's help reaches the Console, a saved dump can be edited in Write, and loaded from the card's options |
| `libwb/` | why2025-linux's drawing library `libwb/` at commit `647c8ea`, as built (the same copy as [ohminous](../../ohminous/source/SOURCES.md)): header-only, MIT; `gen/` the scripts that made its fonts and icons, with the fonts' licences |
| `pm3` | the start script in the package (`files/pm3`) |

Build, with the badge's toolchain on the `PATH`
(`output/host/bin` of the Buildroot build in toolchain/README.md):

```
git clone https://github.com/RfidResearchGroup/proxmark3
cd proxmark3
git checkout da509461b734a61994f8e430e3151e9084bf9718
git am /path/to/source/0*.patch
make client BADGE=1 CC=riscv32-buildroot-linux-uclibc-gcc LIBWB=/path/to/source/libwb -j8
```

This gives `client/proxmark3` (the BFLT program in the package) and
`client/proxmark3.gdb` (the ELF, for `nm` and `size`). The package also
holds seven key lists from `client/dictionaries/` of the same commit:
`mfc_default_keys`, `mfdes_default_keys`, `mfp_default_keys`,
`mfulc_default_keys`, `iclass_default_keys`, `t55xx_default_pwds` and
`ht2_default` (`.dic`).
