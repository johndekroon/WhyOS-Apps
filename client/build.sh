#!/bin/sh
# Build the store for the badge: client/build/store, a static BFLT (docs/client.md).
#
#   client/build.sh               needs a badge toolchain: BR_OUT (default ~/br-badge)
#                                 from why2025-linux Tools/usb/br.sh badge
#
# mbedTLS from deps.sh badge; zlib from the badge build's staging. As
# why2025-linux sdapps/build.sh: -fPIC, elf2flt -r, --no-relax, and a check
# that no absolute jump into the first 2 KB is left. The screens draw with
# libwb: WHY2025_LINUX (default ../../why2025-linux)/libwb.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
BR_OUT=${BR_OUT:-$HOME/br-badge}
T=$BR_OUT/host/bin/riscv32-buildroot-linux-uclibc
cd "$here"
MENU=${WHY2025_LINUX:-$here/../../why2025-linux}/libwb
[ -f "$MENU/wbdraw.h" ] || { echo "build.sh: no $MENU/wbdraw.h (set WHY2025_LINUX)" >&2; exit 1; }
./deps.sh badge
sed -e '/^#/d' -e 's/.*/"&\\n"/' roots.pem > src/roots.inc
mkdir -p build
"$T-gcc" -Os -fPIC -static -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables \
	-Wall -Wno-format-truncation -DSTORE_UI -I.deps/badge/include -Isrc -I"$MENU" -o build/store src/*.c \
	-L.deps/badge/lib -lmbedtls -lmbedx509 -lmbedcrypto -lz \
	-Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r
# its stack: unpacking keeps 48 KB of buffers on it
"$T-flthdr" -s 131072 build/store
if "$T-objdump" -d build/store.gdb | grep -E "\sj(al)?r\s+-?[0-9]+\(zero\)"; then
	echo "build/store: absolute jumps into the first 2 KB" >&2
	exit 1
fi
ls -l build/store
