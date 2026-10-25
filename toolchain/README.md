# Toolchain

Every app in this repository is a static BFLT program built with the
badge's Buildroot toolchain, and every one of them links uClibc-ng. This
directory holds what is needed, together with the public upstream
sources, to rebuild that toolchain and relink or rebuild the apps
(docs/package.md §2.10). The files are copies from why2025-linux at
commit `2be6d7c`.

| File | What |
|---|---|
| `why2025_defconfig` | The Buildroot configuration (`configs/why2025_defconfig` of why2025-linux) |
| `uclibc/0001-linuxthreads-sigaction-vfork-safe.patch` | The badge's change to uClibc-ng (`patches/buildroot/global-patches/uclibc/`) |
| `sdapps-build.sh` | The script that built the apps (`sdapps/build.sh`, copied again at commit `0161ded` on 2026-10-26, with HIDeous, BARbarious, the PDF viewer and OHMinous; the Bus Pirate app it replaced builds only by name); each app's `source/` has its own files |

## Components

| Component | Version | Source |
|---|---|---|
| Buildroot | 2025.02.15 | https://buildroot.org/downloads/buildroot-2025.02.15.tar.xz |
| GCC | 13.x (Buildroot's default for 13) | via Buildroot |
| binutils | 2.43.x | via Buildroot |
| uClibc-ng | 1.0.57, plus Buildroot's patches and the patch above | https://downloads.uclibc-ng.org/releases/1.0.57/ |
| elf2flt | Buildroot's | via Buildroot |

## Rebuilding

1. Unpack Buildroot 2025.02.15. Configure it with `why2025_defconfig`, with
   `BR2_GLOBAL_PATCH_DIR` pointing at a directory that has `uclibc/` from
   here. The defconfig refers to the rest of why2025-linux through
   `@WHY2025_LINUX@` (kernel patches, overlay); the toolchain itself needs
   only the patch above.
2. `make toolchain` gives `output/host/bin/riscv32-buildroot-linux-uclibc-*`.
3. Put an app's `source/` files in `sdapps/<app>/` next to `sdapps-build.sh`,
   set `BR_OUT` to the Buildroot output directory and run
   `sdapps-build.sh <app>`. The script fetches the upstream sources listed
   in each app's `SOURCES.md` and checks their SHA-256.
