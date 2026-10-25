#!/bin/sh
# Build Write for the badge: write/build/write, a static BFLT.
#
#   write/build.sh            needs a badge toolchain: BR_OUT (default ~/br-badge)
#                             from why2025-linux Tools/usb/br.sh badge
#
# As the store's client/build.sh and why2025-linux sdapps/build.sh: -fPIC,
# elf2flt -r, --no-relax, and a check that no absolute jump into the first
# 2 KB is left. The screens draw with the menu's code: WHY2025_LINUX
# (default ../../why2025-linux).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
BR_OUT=${BR_OUT:-$HOME/br-badge}
T=$BR_OUT/host/bin/riscv32-buildroot-linux-uclibc
cd "$here"
MENU=${WHY2025_LINUX:-$here/../../why2025-linux}/patches/buildroot/overlay-src
[ -f "$MENU/wbdraw.h" ] || { echo "build.sh: no $MENU/wbdraw.h (set WHY2025_LINUX)" >&2; exit 1; }
mkdir -p build
"$T-gcc" -Os -fPIC -static -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables \
	-Wall -Wno-format-truncation -Wno-unused-function -Isrc -I"$MENU" -o build/write src/*.c \
	-Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r
# its stack: the layout keeps 512 rows of a line on it in places (layout.h)
"$T-flthdr" -s 65536 build/write
if "$T-objdump" -d build/write.gdb | grep -E "\sj(al)?r\s+-?[0-9]+\(zero\)"; then
	echo "build/write: absolute jumps into the first 2 KB" >&2
	exit 1
fi
ls -l build/write
