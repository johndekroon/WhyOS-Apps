#!/bin/sh
# Build mbedTLS for the store client (docs/client.md §3), pinned:
#
#   client/deps.sh host     static libraries for the host build (tests, CI)
#   client/deps.sh badge    the same for the badge, with its toolchain
#
# Into client/.deps/<target>/{include,lib}. The same release as
# why2025-linux sdapps/build.sh (NetSurf). The badge build needs a badge
# toolchain: BR_OUT (default ~/br-badge) from why2025-linux Tools/usb/br.sh.
set -eu
target=$1
here=$(cd "$(dirname "$0")" && pwd)
URL=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.6/mbedtls-3.6.6.tar.bz2
SHA=8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a
dl=$here/.deps/dl
out=$here/.deps/$target
src=$here/.deps/src-$target
[ -f "$out/lib/libmbedtls.a" ] && exit 0
mkdir -p "$dl" "$out/lib"
tarball=$dl/mbedtls-3.6.6.tar.bz2
[ -f "$tarball" ] || curl -sSfL -o "$tarball" "$URL"
if command -v sha256sum >/dev/null; then
	echo "$SHA  $tarball" | sha256sum -c --quiet -
else
	[ "$(shasum -a 256 "$tarball" | cut -d' ' -f1)" = "$SHA" ] || { echo "bad SHA-256: $tarball" >&2; exit 1; }
fi
rm -rf "$src"
mkdir -p "$src"
tar -C "$src" -xjf "$tarball" --strip-components=1
case $target in
	host)
		cc=${CC:-cc} ar=${AR:-ar}
		cflags="-O2 -g -fPIC" ;;
	badge)
		T=${BR_OUT:-$HOME/br-badge}/host/bin/riscv32-buildroot-linux-uclibc
		cc=$T-gcc ar=$T-ar
		cflags="-Os -fPIC -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables" ;;
	*) echo "usage: deps.sh host|badge" >&2; exit 2 ;;
esac
make -s -C "$src/library" CC="$cc" AR="$ar" CFLAGS="$cflags -I../include" -j4 static
cp "$src"/library/*.a "$out/lib/"
rm -rf "$out/include"
cp -r "$src/include" "$out/include"
echo "mbedTLS 3.6.6 for $target in $out"
