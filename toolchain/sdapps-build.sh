#!/usr/bin/env bash
# Build the apps that live on the SD card (sdapps/README.md), in the build
# VM, with the toolchain of a badge build (Tools/usb/br.sh badge first).
#
#   sdapps/build.sh [gnuboy] [netsurf] [nmap] [imgview] [links] [nethack]
#                   [doom] [sensorpanel] [buspirate]                       default: all
#
# Sources are fetched at pinned versions into ~/sdapps/src; the result is
# ~/br-badge/sdapps/badge/apps/<app>/, to copy to the card's badge/apps/.
# Programs are static BFLT, as Buildroot links packages for this target
# (-fPIC, elf2flt -r), with a stack of their own size (elf2flt -s).
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
BR_OUT=${BR_OUT:-$HOME/br-badge}
T=$BR_OUT/host/bin/riscv32-buildroot-linux-uclibc
SRC=$HOME/sdapps/src
DEST=$BR_OUT/sdapps/badge/apps
CC=$T-gcc
CFLAGS=(-Os -fPIC -static -ffunction-sections -fdata-sections -w)
# --no-relax: the RISC-V linker turns a call or load of a symbol in the
# first 2 KB into an absolute "jalr N(zero)", which has no relocation;
# in a FLAT program, loaded elsewhere, it jumps into nothing. NetSurf has
# zlib there: it crashed at its first PNG.
flat() { LDFLAT=(-Wl,--gc-sections -Wl,--no-relax "-Wl,-elf2flt=-r -s $1"); }	# stack bytes

# Fail on what --no-relax should have prevented: an absolute call into
# the program's first 2 KB (a load like "lw a5,0(zero)" is the compiler's
# own NULL dereference, not the linker's).
zero_page() {	# program.gdb
	if "$T-objdump" -d "$1" | grep -E "\sj(al)?r\s+-?[0-9]+\(zero\)"; then
		echo "$1: absolute jumps into the first 2 KB (see flat())" >&2
		exit 1
	fi
}

# Libraries NetSurf needs that the badge build does not have, at the
# versions Buildroot 2025.02 packages (and its sha256 sums), static, in
# a prefix of their own: they are linked into NetSurf, not installed.
P=$HOME/sdapps/prefix
LIBCFLAGS="-Os -fPIC -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables"
DEPS=(
	"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.6/mbedtls-3.6.6.tar.bz2 8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a"
	"https://curl.se/download/curl-8.20.0.tar.xz 63fe2dc148ba0ceae89922ef838f7e5c946272c2e78b7c59fab4b79d3ce2b896"
	"http://downloads.sourceforge.net/project/libpng/libpng16/1.6.58/libpng-1.6.58.tar.xz 28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775"
	"https://github.com/libexpat/libexpat/releases/download/R_2_8_1/expat-2.8.1.tar.xz 10b195ee78160a908388180a8fe3603d4e9a12f4755fbf5f3816b23a9d750da0"
	"http://www.ijg.org/files/jpegsrc.v9f.tar.gz 04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b"
)

get() {	# url sha256 -> unpacked in $SRC, prints its directory
	local url=$1 sum=$2 f=$SRC/dl/${1##*/}
	mkdir -p "$SRC/dl"
	[ -f "$f" ] || curl -sSfL -o "$f" "$url"
	echo "$sum  $f" | sha256sum -c --quiet - >&2
	tar -C "$SRC" -xf "$f"
	tar -tf "$f" | sed -n 1p | cut -d/ -f1	# sed reads it all: no SIGPIPE
}

cfg() {	# configure a library for the prefix
	./configure --host=riscv32-buildroot-linux-uclibc --prefix="$P" --disable-shared \
		--enable-static CC="$CC" AR="$T-ar" RANLIB="$T-ranlib" CFLAGS="$LIBCFLAGS" "$@" >/dev/null
}

build_deps() {
	local d
	[ -f "$P/.done" ] && return
	mkdir -p "$P/lib" "$P/include"
	d=$(get ${DEPS[0]})
	make -s -C "$SRC/$d/library" CC="$CC" AR="$T-ar" CFLAGS="$LIBCFLAGS -I../include" static
	cp "$SRC/$d"/library/*.a "$P/lib/"
	cp -r "$SRC/$d/include/mbedtls" "$SRC/$d/include/psa" "$P/include/"
	d=$(get ${DEPS[1]})
	(cd "$SRC/$d" && cfg --with-mbedtls="$P" --with-zlib --without-libpsl --without-brotli \
		--without-zstd --without-nghttp2 --without-libidn2 --disable-threaded-resolver \
		--disable-ldap --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
		--disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt \
		--disable-smb --disable-manual --disable-docs --disable-ntlm --disable-ws \
		--without-ca-bundle --without-ca-path && \
		sed -i "s|^#define HAVE_EVENTFD 1|/* no eventfd: CONFIG_EVENTFD is off in the badge kernel */|" \
			lib/curl_config.h && make -s -C lib && make -s -C lib install \
		&& make -s -C include install && install -m 644 libcurl.pc "$P/lib/pkgconfig/" 2>/dev/null \
		|| { mkdir -p "$P/lib/pkgconfig"; install -m 644 libcurl.pc "$P/lib/pkgconfig/"; })
	d=$(get ${DEPS[2]})
	(cd "$SRC/$d" && cfg && make -s install)
	d=$(get ${DEPS[3]})
	(cd "$SRC/$d" && cfg --without-docbook --without-examples --without-tests && make -s -C lib install && \
		make -s install-pkgconfigDATA 2>/dev/null; true)
	d=$(get ${DEPS[4]})
	(cd "$SRC/$d" && cfg && make -s libjpeg.la && make -s install-libLTLIBRARIES install-includeHEADERS && install -m 644 jconfig.h "$P/include/")
	touch "$P/.done"
	ls -l "$P/lib"
}

GNUBOY_REPO=https://github.com/rofl0r/gnuboy
GNUBOY_REV=c367bb4

fetch() {	# dir repo rev
	mkdir -p "$SRC"
	[ -d "$SRC/$1/.git" ] || git clone -q "$2" "$SRC/$1"
	git -C "$SRC/$1" checkout -q "$3"
}

build_gnuboy() {
	local s=$SRC/gnuboy d=$DEST/gnuboy objs=()
	fetch gnuboy "$GNUBOY_REPO" "$GNUBOY_REV"
	mkdir -p "$s/sys/badge" "$d"
	cp "$HERE/gnuboy/badge.c" "$s/sys/badge/badge.c"
	for f in lcd refresh lcdc palette cpu mem rtc hw sound events keytable menu \
		 loader save debug emu main rccmds rckeys rcvars rcfile exports split \
		 path inflate miniz_tinfl xz/xz_crc32 xz/xz_crc64 xz/xz_dec_lzma2 \
		 xz/xz_dec_stream xz/xz_dec_bcj sys/nix/nix sys/dummy/nosound \
		 sys/dummy/nojoy sys/badge/badge; do
		objs+=("$s/$f.c")
	done
	flat 65536
	"$CC" "${CFLAGS[@]}" -O2 -DIS_LITTLE_ENDIAN -DIS_LINUX -I"$s" -I"$s/sys/nix" \
		"${objs[@]}" "${LDFLAT[@]}" -o "$d/gnuboy"
	zero_page "$d/gnuboy.gdb"
	cp "$HERE/gnuboy/badge.rc" "$HERE/gnuboy/app.conf" "$d/"
	ls -l "$d"
}

NETSURF_URL=https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz
NETSURF_SHA=x

build_netsurf() {
	local s=$SRC/netsurf-all-3.11 d=$DEST/netsurf
	build_deps
	[ -d "$s" ] || (cd "$SRC" && curl -sSfLO "$NETSURF_URL" && tar xzf netsurf-all-3.11.tar.gz)
	cp "$HERE/netsurf/badge.c" "$s/libnsfb/src/surface/badge.c"
	grep -q badge.c "$s/libnsfb/src/surface/Makefile" ||
		sed -i 's/^SURFACE_HANDLER_yes := surface.c ram.c$/& badge.c/' "$s/libnsfb/src/surface/Makefile"
	cp "$HERE/netsurf/Makefile.config" "$s/netsurf/Makefile.config"
	# The file fetcher maps local files MAP_SHARED, which NOMMU allows only
	# on filesystems that can be mapped directly, not FAT: every file://
	# fetch failed, the error page's own stylesheets failed too, and
	# NetSurf loaded that page again and again until memory ran out.
	# Without HAVE_MMAP it read()s them.
	sed -i 's|^#define HAVE_MMAP$|/* not on the badge: MAP_SHARED of a FAT file fails on NOMMU */|' \
		"$s/netsurf/utils/config.h"
	# source patches (netsurf/*.patch), once
	for f in "$HERE"/netsurf/*.patch; do
		patch -d "$s" -p1 -s -f -R --dry-run < "$f" >/dev/null 2>&1 ||
			patch -d "$s" -p1 -s < "$f"
	done
	# a failed allocation ends NetSurf at once (netsurf/oom.c)
	"$CC" $LIBCFLAGS -c "$HERE/netsurf/oom.c" -o "$SRC/oom.o"
	# a small iconv in place of GNU libiconv (netsurf/iconv.c): first on
	# the library path, so -liconv finds it
	"$CC" $LIBCFLAGS -c "$HERE/netsurf/iconv.c" -o "$SRC/iconv.o"
	rm -f "$P/lib/libiconv.a"
	"$T-ar" rcs "$P/lib/libiconv.a" "$SRC/iconv.o"
	cp "$HERE/netsurf/iconv.h" "$P/include/iconv.h"
	mkdir -p "$d" "$SRC/xbin"
	# NetSurf's buildsystem takes the realpath of $HOST-gcc, which for
	# Buildroot is its shared toolchain-wrapper (it goes by its name): give
	# it scripts that stay themselves.
	for t in gcc ar ranlib strip objcopy ld nm; do
		printf '#!/bin/sh\nexec %s "$@"\n' "$T-$t" > "$SRC/xbin/riscv32-buildroot-linux-uclibc-$t"
		chmod +x "$SRC/xbin/riscv32-buildroot-linux-uclibc-$t"
	done
	# libnsfb registers its surfaces from constructors: -u pulls badge.o out
	# of the static library, which nothing else would.
	# CFLAGS and LDFLAGS from the environment: on the command line they
	# would replace the makefiles' own instead of adding to them. uClibc
	# has the GNU extensions (strchrnul, ...) NetSurf otherwise brings.
	# netsurf-all builds once per stamp; without it the parts rebuild what
	# changed (a new patch, say)
	rm -f "$s/inst-framebuffer/build-stamp"
	PATH=$SRC/xbin:$PATH PKG_CONFIG_PATH=$P/lib/pkgconfig CC=riscv32-buildroot-linux-uclibc-gcc \
	CFLAGS="$LIBCFLAGS -I$P/include -D_GNU_SOURCE" \
	LDFLAGS="-L$P/lib -static -Wl,-u,badge_rtns -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r \
		$SRC/oom.o -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc" \
	make -C "$s" TARGET=framebuffer HOST=riscv32-buildroot-linux-uclibc "$@"
	zero_page "$s/netsurf/nsfb.gdb"
	# its stack (elf2flt -s has a space make would split)
	"$T-flthdr" -s 262144 "$s/netsurf/nsfb"
	cp "$s/netsurf/nsfb" "$d/netsurf"
	# what it reads at run time, without symlinks (the card is FAT)
	mkdir -p "$d/res"
	for f in adblock.css credits.html default.css internal.css licence.html \
		 netsurf.png quirks.css welcome.html Messages; do
		cp -L "$s/netsurf/frontends/framebuffer/res/$f" "$d/res/$f"
	done
	cp "$HERE/netsurf/Choices" "$d/res/Choices"
	cp "$HERE/netsurf/app.conf" "$d/app.conf"
	# Mozilla's CA list as curl publishes it, for HTTPS
	[ -f "$SRC/dl/cacert.pem" ] || curl -sSfL -o "$SRC/dl/cacert.pem" https://curl.se/ca/cacert.pem
	cp "$SRC/dl/cacert.pem" "$d/ca-bundle.crt"
	ls -l "$d" "$d/res"
}

# nmap is C++: the badge's toolchain with C++ added (Tools/usb/br.sh cxx),
# a separate Buildroot output that only builds the toolchain. nmap 7.95 is
# what Buildroot 2025.02 packages (its sha256), with its own libpcap,
# libdnet, liblinear and pcre; no Lua (NSE), OpenSSL, libssh2, zlib, ncat
# or nping. Buildroot does not offer it here because it "depends on
# BR2_USE_MMU (fork)": nmap itself does not fork.
NMAP_URL=https://nmap.org/dist/nmap-7.95.tar.bz2
NMAP_SHA=e14ab530e47b5afd88f1c8a2bac7f89cd8fe6b478e22d255c5b9bddb7a1c5778
CXX_OUT=${CXX_OUT:-$HOME/br-cxx}

build_nmap() {
	local s d=$DEST/nmap X=$CXX_OUT/host/bin/riscv32-buildroot-linux-uclibc f
	local F="-Os -fPIC -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables"
	[ -x "$X-g++" ] || { echo "no C++ toolchain: Tools/usb/br.sh cxx" >&2; exit 1; }
	s=$SRC/$(get "$NMAP_URL" "$NMAP_SHA")
	patch -d "$s" -p1 -s -f -N < "$HOME/buildroot/package/nmap/0001-libdnet-always-build-a-static-library.patch" \
		>/dev/null || true
	# Without IPv6 OS detection's model: FPmean, FPvariance and the weights
	# are 1.5 MB of initialised data, loaded into RAM with the program (no
	# XIP on NOMMU), and OS detection needs raw packets, which the badge
	# cannot capture (libpcap, see README). Classes set to 0, so nothing
	# would index the emptied tables.
	python3 - "$s/FPModel.cc" <<'PY'
import re, sys
p = sys.argv[1]; s = open(p).read()
s = re.sub(r"double FPmean\[\]\[695\] = \{.*?\n\};", "double FPmean[1][695];", s, flags=re.S)
s = re.sub(r"double FPvariance\[\]\[695\] = \{.*?\n\};", "double FPvariance[1][695];", s, flags=re.S)
s = re.sub(r"static double _w\[\] = \{.*?\};", "static double _w[1];", s, flags=re.S)
s = s.replace("\t{0},\n\t92,\n\t695,", "\t{0},\n\t0,\t/* nr_class: the model is left out (sdapps/build.sh) */\n\t695,")
open(p, "w").write(s)
PY
	# its config.sub predates riscv32-buildroot-*
	for f in $(find "$s" -name config.sub -o -name config.guess); do
		cp "$HOME/buildroot/support/gnuconfig/${f##*/}" "$f"
	done
	# _exit instead of exit: no slow freeing at the end (nmap/fastexit.c)
	"$X-gcc" $F -c "$HERE/nmap/fastexit.c" -o "$SRC/fastexit.o"
	(cd "$s" && ./configure --host=riscv32-buildroot-linux-uclibc --build=x86_64-linux-gnu \
		CC="$X-gcc" CXX="$X-g++" AR="$X-ar" RANLIB="$X-ranlib" CFLAGS="$F" CXXFLAGS="$F" \
		LDFLAGS="-static -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r -Wl,--wrap=exit $SRC/fastexit.o" \
		--without-liblua --without-zenmap --without-ndiff --without-ncat --without-nping \
		--without-openssl --without-libssh2 --without-libz --with-libdnet=included \
		--with-libpcap=included --with-libpcre=included --with-liblinear=included >/dev/null)
	# the archive's timestamps make the bundled pcre want aclocal: not needed
	make -s -C "$s" -j"$(nproc)" nmap ACLOCAL=true AUTOMAKE=true AUTOCONF=true AUTOHEADER=true
	zero_page "$s/nmap.gdb"
	"$X-flthdr" -s 262144 "$s/nmap"
	# its data, without nmap-os-db: -O needs raw packets
	rm -rf "$d"
	mkdir -p "$d/share"
	cp "$s/nmap" "$d/nmap"
	for f in nmap-protocols nmap-mac-prefixes nmap-rpc nmap-service-probes; do
		cp "$s/$f" "$d/share/"
	done
	# The 3000 most common ports of its 27 000 (others show as "unknown"):
	# the whole table took 3 MB and two seconds to read, and was what made
	# exit slow (fastexit.c). -F and --top-ports up to 3000 are unchanged.
	{ grep '^#' "$s/nmap-services"
	  grep -v '^#' "$s/nmap-services" | sort -t "$(printf '\t')" -k3,3gr | sed -n 1,3000p; } \
		> "$d/share/nmap-services"
	cp "$HERE/nmap/app.conf" "$d/app.conf"
	ls -l "$d" "$d/share"
}

# imgview: Files' picture viewer (imgview/imgview.c), with the libjpeg and
# libpng built for NetSurf, and the menu's drawing code and fonts.
build_imgview() {
	local d=$DEST/imgview O=$HERE/../patches/buildroot/overlay-src
	build_deps
	mkdir -p "$d"
	flat 65536
	"$CC" "${CFLAGS[@]}" -Wall -I"$O" -I"$P/include" "$HERE/imgview/imgview.c" \
		-L"$P/lib" -ljpeg -lpng16 -lz -lm "${LDFLAT[@]}" -o "$d/imgview"
	zero_page "$d/imgview.gdb"
	rm -f "$d/imgview.gdb"
	cp "$HERE/imgview/app.conf" "$d/"
	ls -l "$d"
}

# Links: a text browser, in appterm's window (links/app.conf). It keeps a
# page as formatted lines, not as a DOM and a box tree, and does next to
# nothing with CSS: NetSurf needed 6 MB more for Wikipedia's main page
# than the badge has. HTTPS through OpenSSL (the version Buildroot 2025.02
# packages), static and cut down, in a prefix of its own.
LINKS_URL=http://links.twibright.com/download/links-2.30.tar.bz2
LINKS_SHA=c4631c6b5a11527cdc3cb7872fc23b7f2b25c2b021d596be410dadb40315f166
OPENSSL_URL=https://github.com/openssl/openssl/releases/download/openssl-3.5.6/openssl-3.5.6.tar.gz
OPENSSL_SHA=deae7c80cba99c4b4f940ecadb3c3338b13cb77418409238e57d7f31f2a3b736
LP=$HOME/sdapps/prefix-links

build_openssl() {
	local s
	[ -f "$LP/lib/libssl.a" ] && return
	s=$SRC/$(get "$OPENSSL_URL" "$OPENSSL_SHA")
	# --openssldir: OpenSSL's default CA file is <dir>/cert.pem, so Links
	# checks certificates without options. Old and rare algorithms left
	# out: the whole library is loaded into RAM with Links.
	(cd "$s" && ./Configure linux-generic32 --prefix="$LP" --libdir=lib \
		--openssldir=/mnt/sd/badge/apps/links no-shared no-threads \
		no-asm no-async no-dso no-engine no-tests no-apps no-docs no-comp no-ui-console \
		no-quic no-ssl3 no-dtls no-srp no-cms no-ts no-afalgeng no-legacy no-idea \
		no-camellia no-seed no-bf no-cast no-des no-rc2 no-rc4 no-md4 no-mdc2 \
		no-whirlpool no-sm2 no-sm3 no-sm4 no-gost no-siphash no-ocb no-aria no-ct \
		no-srtp no-sctp no-scrypt no-ec2m no-dsa no-nextprotoneg \
		CC="$CC" AR="$T-ar" RANLIB="$T-ranlib" CFLAGS="$LIBCFLAGS" >/dev/null &&
		make -s -j"$(nproc)" build_libs && make -s install_dev)
}

build_links() {
	local s d=$DEST/links
	build_openssl
	s=$SRC/$(get "$LINKS_URL" "$LINKS_SHA")
	for f in "$HERE"/links/*.patch; do	# get unpacks afresh: apply each
		patch -d "$s" -p1 -s < "$f"
	done
	"$CC" $LIBCFLAGS -c "$HERE/links/nofork.c" -o "$SRC/nofork.o"
	# NO_ASYNC_LOOKUP: DNS in the program itself, not in a forked helper
	# (no fork on NOMMU); NO_CTRL_Z: suspending forks a helper too; the
	# other forks fail (links/nofork.c).
	cp "$HOME/buildroot/support/gnuconfig/config.sub" "$HOME/buildroot/support/gnuconfig/config.guess" "$s/"
	(cd "$s" && CC="$CC" CFLAGS="$LIBCFLAGS -DNO_ASYNC_LOOKUP -DNO_CTRL_Z" \
		LDFLAGS="-static -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r" LIBS="$SRC/nofork.o" \
		./configure --host=riscv32-buildroot-linux-uclibc \
		--without-x --without-fb --without-directfb --without-svgalib --without-gpm \
		--without-brotli --without-zstd --without-bzip2 --without-lzma --without-ipv6 \
		--without-libjpeg --without-libtiff --without-librsvg --disable-graphics \
		--with-ssl="$LP" --disable-ssl-pkgconfig >/dev/null)
	make -s -C "$s" -j"$(nproc)"
	zero_page "$s/links.gdb"
	"$T-flthdr" -s 262144 "$s/links"	# its stack (configure splits -s N)
	mkdir -p "$d"
	cp "$s/links" "$d/links"
	# Mozilla's CA list (curl's copy) as OpenSSL's hashed directory,
	# <openssldir>/certs/<subject hash>.0: a connection loads only the CA
	# it needs; the list as one cert.pem was parsed whole for each.
	[ -f "$SRC/dl/cacert.pem" ] || curl -sSfL -o "$SRC/dl/cacert.pem" https://curl.se/ca/cacert.pem
	rm -rf "$d/certs" "$d/cert.pem"
	mkdir -p "$d/certs"
	python3 - "$SRC/dl/cacert.pem" "$d/certs" <<'PY'
import re, subprocess, sys
pems = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----\n", open(sys.argv[1]).read(), re.S)
for pem in pems:
    h = subprocess.run(["openssl", "x509", "-noout", "-subject_hash"], input=pem, text=True,
                       capture_output=True, check=True).stdout.strip()
    n = 0
    while True:
        try:
            open("%s/%s.%d" % (sys.argv[2], h, n), "x").write(pem)
            break
        except FileExistsError:
            n += 1
PY
	cp "$HERE/links/app.conf" "$HERE/links/start" "$HERE/links/start.html" "$d/"
	ls -l "$d"
}

# NetHack 3.6.7 with its tty interface, in appterm's window (nethack/
# app.conf); the map is 80 columns and scrolls (CLIPPING) in appterm's 73.
# Source changes in nethack/0001-badge.patch: termcap through NetHack's
# own tclib.c (no ncurses here), saves and scores in APP_DATA, SIGTERM
# saves the game, the arrow keys move (nethack/badge.c); no shell, mail,
# suspend or compress program (no fork), zlib compresses the saves.
# makedefs, dgn_comp, lev_comp and dlb run during the build and write
# structs with longs into the level files: they are built for the badge
# as static ELF and run through qemu-user (binfmt), so the data has the
# badge's sizes. The VM needs qemu-user.
NETHACK_URL=https://www.nethack.org/download/3.6.7/nethack-367-src.tgz
NETHACK_SHA=98cf67df6debf9668a61745aa84c09bcab362e5d33f5b944ec5155d44d2aacb2

build_nethack() {
	local s d=$DEST/nethack Z=$BR_OUT/staging/usr
	[ -e /proc/sys/fs/binfmt_misc/qemu-riscv32 ] || { echo "no qemu-user: apt install qemu-user" >&2; exit 1; }
	s=$SRC/$(get "$NETHACK_URL" "$NETHACK_SHA")
	patch -d "$s" -p1 -s < "$HERE/nethack/0001-badge.patch"
	cp "$s/sys/unix/Makefile.top" "$s/Makefile"
	cp "$s/sys/unix/Makefile.dat" "$s/dat/Makefile"
	cp "$s/sys/unix/Makefile.src" "$s/src/Makefile"
	cp "$s/sys/unix/Makefile.utl" "$s/util/Makefile"
	cp "$s/sys/unix/Makefile.doc" "$s/doc/Makefile"
	local nf=(CC="$CC" LINK="$CC" YACC="bison -y" LEX=flex
		CFLAGS="$LIBCFLAGS -I../include -I$Z/include -DDLB -DZLIB_COMP -DNOMAIL -DNOSHELL")
	# the tools, as ELF for qemu-user
	make -s -C "$s/util" "${nf[@]}" LFLAGS="-static" makedefs dgn_comp lev_comp dlb
	# the game: BFLT, with tclib (termcap), the badge glue and zlib
	"$CC" $LIBCFLAGS -I"$s/include" -I"$Z/include" -DDLB -DZLIB_COMP -DNOMAIL -DNOSHELL \
		-c "$s/sys/share/tclib.c" -o "$s/src/tclib.o"
	"$CC" $LIBCFLAGS -I"$s/include" -I"$Z/include" -DDLB -DZLIB_COMP -DNOMAIL -DNOSHELL \
		-c "$HERE/nethack/badge.c" -o "$s/src/badge.o"
	make -s -C "$s/src" "${nf[@]}" WINOBJ='$(WINTTYOBJ)' WINTTYLIB= LIBS="tclib.o badge.o -L$Z/lib -lz" \
		LFLAGS="-static -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r" nethack
	zero_page "$s/src/nethack.gdb"
	"$T-flthdr" -s 131072 "$s/src/nethack"	# its stack (make splits -s N)
	# the data: level files, the dungeon, then all of it in one nhdat
	make -s -C "$s/dat" "${nf[@]}" all
	make -s -C "$s" "${nf[@]}" dlb
	rm -rf "$d"
	mkdir -p "$d"
	cp "$s/src/nethack" "$s/dat/nhdat" "$s/dat/license" "$d/"
	cp "$HERE/nethack/start" "$HERE/nethack/termcap" "$HERE/nethack/sysconf" \
		"$HERE/nethack/nethackrc" "$HERE/nethack/app.conf" "$d/"
	ls -l "$d"
}

# The sensor panel: BME680 and BMI270 readings, live (it was in the image).
build_sensorpanel() {
	local d=$DEST/sensorpanel
	mkdir -p "$d"
	flat 16384
	"$CC" "${CFLAGS[@]}" -Wall "$HERE/sensorpanel/sensorpanel.c" "${LDFLAT[@]}" -o "$d/sensorpanel"
	zero_page "$d/sensorpanel.gdb"
	rm -f "$d/sensorpanel.gdb"
	cp "$HERE/sensorpanel/app.conf" "$d/"
	ls -l "$d"
}

# The Bus Pirate 6 screens of the start menu, on their own: the menu's
# source with -DBP_APP (the menu itself leaves them out, -DHAS_BP=0).
build_buspirate() {
	local d=$DEST/buspirate O=$HERE/../patches/buildroot/overlay-src
	mkdir -p "$d"
	flat 16384
	"$CC" "${CFLAGS[@]}" -Wall -DBP_APP -I"$O" "$O/home.c" "${LDFLAT[@]}" -o "$d/buspirate"
	zero_page "$d/buspirate.gdb"
	rm -f "$d/buspirate.gdb"
	cp "$HERE/buspirate/app.conf" "$d/"
	ls -l "$d"
}

# DOOM: fbDOOM (no SDL, the framebuffer and the VT's keys) with our patch
# (global-patches/fbdoom), the shareware WAD, and doom.c to start it. Was
# a Buildroot package in the image (buildroot-tree/0002) until 2026-10-12.
FBDOOM_REV=17280163bc95e5d954d2efaa0633489b763b4cd1
FBDOOM_SHA=77f57cee68fed438dffdba96f6070b8975c16652a63ddf4fb967994e5585a38a
WAD_URL=http://www.jbserver.com/downloads/games/doom/misc/shareware/doom19s.zip
WAD_SHA=cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6

build_doom() {
	local d=$DEST/doom s=$SRC/fbDOOM-$FBDOOM_REV f=$SRC/dl/fbdoom-$FBDOOM_REV.tar.gz w=$SRC/dl/doom19s.zip
	mkdir -p "$SRC/dl" "$d"
	[ -f "$f" ] || curl -sSfL -o "$f" "https://github.com/maximevince/fbDOOM/archive/$FBDOOM_REV.tar.gz"
	echo "$FBDOOM_SHA  $f" | sha256sum -c --quiet -
	rm -rf "$s"
	tar -C "$SRC" -xzf "$f"
	for p in "$HERE"/../patches/buildroot/global-patches/fbdoom/*.patch; do
		patch -s -d "$s" -p1 < "$p"
	done
	# its Makefile takes link flags only through CC, where "-s N" would
	# split: the stack (DOOM recurses; 64 KB as the package had) via flthdr
	make -s -C "$s/fbdoom" CC="$CC ${CFLAGS[*]} -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r" \
		NOSDL=1 fbdoom
	zero_page "$s/fbdoom/fbdoom.gdb"
	"$T-flthdr" -s 65536 "$s/fbdoom/fbdoom"
	cp "$s/fbdoom/fbdoom" "$d/"
	flat 16384
	"$CC" "${CFLAGS[@]}" -Wall "$HERE/doom/doom.c" "${LDFLAT[@]}" -o "$d/doom"
	zero_page "$d/doom.gdb"
	rm -f "$d/doom.gdb"
	[ -f "$w" ] || curl -sSfL -o "$w" "$WAD_URL"
	echo "$WAD_SHA  $w" | sha256sum -c --quiet -
	rm -rf "$SRC/wad" && mkdir -p "$SRC/wad"
	unzip -p "$w" 'DOOMS_19.[12]' > "$SRC/wad/doom.zip"
	unzip -q -o -d "$SRC/wad" "$SRC/wad/doom.zip" DOOM1.WAD README.TXT
	cp "$SRC/wad/DOOM1.WAD" "$d/doom1.wad"
	cp "$s/README.TXT" "$d/README-fbdoom.txt"
	cp "$SRC/wad/README.TXT" "$d/README-shareware.txt"
	cp "$HERE/doom/app.conf" "$d/"
	ls -l "$d"
}

for app in "${@:-gnuboy netsurf nmap imgview links nethack doom sensorpanel buspirate}"; do
	for a in $app; do
		case $a in
		gnuboy) build_gnuboy ;;
		deps) build_deps ;;
		netsurf) build_netsurf ;;
		nmap) build_nmap ;;
		imgview) build_imgview ;;
		links) build_links ;;
		nethack) build_nethack ;;
		doom) build_doom ;;
		sensorpanel) build_sensorpanel ;;
		buspirate) build_buspirate ;;
		*) echo "unknown app $a" >&2; exit 2 ;;
		esac
	done
done
