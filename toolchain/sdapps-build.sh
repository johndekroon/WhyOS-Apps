#!/usr/bin/env bash
# Build the apps that live on the SD card (sdapps/README.md), in the build
# VM, with the toolchain of a badge build (WhyOS BUILDING.md first).
#
#   sdapps/build.sh [gnuboy] [netsurf] [nmap] [imgview] [links] [nethack]
#                   [doom] [sensorpanel] [nano] [calc] [hideous] [barbarious] [pdfview]
#                   [ohminous] [flashlight] [opentyrian] [duke3d] [keen4]
#                   [micropython] [tinyalsa] [airplay] [vnc]
#                                                                          default: all
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
	sed -i 's/^static char \*saveprefix;/char *saveprefix;/' "$s/loader.c"	# for badge.c's backup
	cp "$HERE/gnuboy/badge.c" "$HERE/pause/pause.c" "$HERE/pause/pause.h" "$s/sys/badge/"
	for f in lcd refresh lcdc palette cpu mem rtc hw sound events keytable menu \
		 loader save debug emu main rccmds rckeys rcvars rcfile exports split \
		 path inflate miniz_tinfl xz/xz_crc32 xz/xz_crc64 xz/xz_dec_lzma2 \
		 xz/xz_dec_stream xz/xz_dec_bcj sys/nix/nix \
		 sys/dummy/nojoy sys/badge/badge sys/badge/pause; do
		objs+=("$s/$f.c")
	done
	flat 65536
	# pause.c (sdapps/pause) draws with libwb, the menu's drawing library;
	# badge.c plays the sound through bsound (sound/bsound.h), whose
	# tinyalsa has a pcm_close of its own
	bsound
	"$CC" "${CFLAGS[@]}" -O2 -DIS_LITTLE_ENDIAN -DIS_LINUX -Dpcm_close=gnuboy_pcm_close \
		-I"$s" -I"$s/sys/nix" -I"$HERE/sound" \
		-I"$HERE/../patches/buildroot/overlay-src" -I"$HERE/../libwb" "${objs[@]}" "$SRC/bsound.o" \
		"${LDFLAT[@]}" $BSOUND_LIBS -o "$d/gnuboy"
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
	cp "$HERE/netsurf/badge.c" "$HERE/pause/pause.h" "$s/libnsfb/src/surface/"
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
	# Esc's pause screen (sdapps/pause), drawn with the menu's wbdraw.h
	"$CC" $LIBCFLAGS -I"$HERE/../patches/buildroot/overlay-src" -I"$HERE/../libwb" -c "$HERE/pause/pause.c" \
		-o "$SRC/pause.o"
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
		$SRC/oom.o $SRC/pause.o -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc" \
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

# nmap is C++: the badge's toolchain with C++ added (WhyOS BUILDING.md),
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
	[ -x "$X-g++" ] || { echo "no C++ toolchain: see WhyOS BUILDING.md" >&2; exit 1; }
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
	"$CC" "${CFLAGS[@]}" -Wall -I"$O" -I"$HERE/../libwb" -I"$P/include" "$HERE/imgview/imgview.c" \
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
	# recover: a save from the level files a killed game left (start)
	make -s -C "$s/util" "${nf[@]}" LFLAGS="-static -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r" recover
	zero_page "$s/util/recover.gdb"
	"$T-flthdr" -s 32768 "$s/util/recover"
	# the data: level files, the dungeon, then all of it in one nhdat
	make -s -C "$s/dat" "${nf[@]}" all
	make -s -C "$s" "${nf[@]}" dlb
	rm -rf "$d"
	mkdir -p "$d"
	cp "$s/src/nethack" "$s/util/recover" "$s/dat/nhdat" "$s/dat/license" "$d/"
	cp "$HERE/nethack/start" "$HERE/nethack/termcap" "$HERE/nethack/sysconf" \
		"$HERE/nethack/nethackrc" "$HERE/nethack/app.conf" "$d/"
	ls -l "$d"
}

# nano 9.2, the text editor, in appterm's window (nano/app.conf) and as
# the nano command. ncurses 6.6 is linked in, without terminfo files:
# the terminals it knows are built into it (--with-fallbacks), among them
# appterm's own entry (nano/appterm.ti; the start script picks it), which
# a host tic of the same version compiles from ncurses' terminfo.src. No UTF-8 (uClibc has no
# locales here), and no fork: the speller, linter and formatter are left
# out, running a command fails (nano/nofork.c).
NCURSES_URL=https://ftp.gnu.org/gnu/ncurses/ncurses-6.6.tar.gz
NCURSES_SHA=355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11
NANO_URL=https://www.nano-editor.org/dist/v9/nano-9.2.tar.xz
NANO_SHA=05ecb99247b782e8a5b3a25ed4101dd034b0236902f7449bc9795b717642f7e9
NANO_TERMS=appterm,vt100,vt102,ansi,linux,xterm,xterm-256color,screen,screen-256color,tmux,tmux-256color

build_ncurses() {
	local n HN=$SRC/ncurses-host NC=$SRC/ncurses-badge
	[ -f "$NC/.done" ] && return
	n=$SRC/$(get "$NCURSES_URL" "$NCURSES_SHA")
	# appterm's entry next to the others: the fallbacks are made from it
	cat "$HERE/nano/appterm.ti" >> "$n/misc/terminfo.src"
	# tic and infocmp for the build machine, with their terminfo files
	rm -rf "$HN" "$n/host" "$n/badge" && mkdir -p "$n/host"
	(cd "$n/host" && ../configure --prefix="$HN" --without-ada --without-cxx --without-cxx-binding \
		--without-manpages --without-tests --without-debug --disable-widec >/dev/null &&
		make -s -j"$(nproc)" >/dev/null && make -s install >/dev/null)
	rm -rf "$NC" && mkdir -p "$n/badge"
	(cd "$n/badge" && ../configure --host=riscv32-buildroot-linux-uclibc \
		--prefix="$NC" --disable-shared --enable-static --without-ada --without-cxx \
		--without-cxx-binding --without-progs --without-tests --without-manpages --without-debug \
		--without-gpm --disable-widec --disable-database --disable-home-terminfo \
		--with-fallbacks="$NANO_TERMS" --with-tic-path="$HN/bin/tic" \
		--with-infocmp-path="$HN/bin/infocmp" \
		CC="$CC" AR="$T-ar" RANLIB="$T-ranlib" CFLAGS="$LIBCFLAGS" >/dev/null &&
		make -s -j"$(nproc)" libs >/dev/null &&
		make -s install.libs install.includes >/dev/null)
	touch "$NC/.done"
}

build_nano() {
	local s d=$DEST/nano NC=$SRC/ncurses-badge
	build_ncurses
	s=$SRC/$(get "$NANO_URL" "$NANO_SHA")
	"$CC" $LIBCFLAGS -c "$HERE/nano/nofork.c" -o "$SRC/nano-nofork.o"
	(cd "$s" && PKG_CONFIG=false NCURSES_CFLAGS="-I$NC/include -I$NC/include/ncurses" \
		NCURSES_LIBS="-L$NC/lib -lncurses" CC="$CC" \
		CFLAGS="$LIBCFLAGS -I$NC/include -I$NC/include/ncurses" CPPFLAGS="-I$NC/include" \
		LDFLAGS="-static -L$NC/lib -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r" \
		LIBS="$SRC/nano-nofork.o" ./configure --host=riscv32-buildroot-linux-uclibc \
		--disable-nls --disable-utf8 --disable-speller --disable-linter --disable-formatter \
		--disable-mouse --disable-libmagic --disable-threads >/dev/null)
	make -s -C "$s" -j"$(nproc)"
	zero_page "$s/src/nano.gdb"
	"$T-flthdr" -s 65536 "$s/src/nano"	# its stack (configure splits -s N)
	rm -rf "$d"
	mkdir -p "$d/syntax"
	cp "$s/src/nano" "$s/COPYING" "$d/"
	cp "$s"/syntax/*.nanorc "$d/syntax/"
	cp "$HERE/nano/start" "$HERE/nano/nanorc" "$HERE/nano/app.conf" "$d/"
	ls -l "$d"
}

TINYALSA_URL=https://github.com/tinyalsa/tinyalsa/archive/v2.0.0/tinyalsa-2.0.0.tar.gz
TINYALSA_SHA=573ae0b2d3480851c1d2a12503ead2beea27f92d44ed47b74b553ba947994ef1

# tinyalsa's tools (tinyplay, tinypcminfo, tinymix) for the speaker: ALSA's
# own alsa-utils need fork, and alsa-lib does not fit in the rootfs. Plain
# ioctls on /dev/snd, no plugins (no dlopen). Into $BR_OUT/sdapps/tinyalsa.
build_tinyalsa() {
	local s d=$BR_OUT/sdapps/tinyalsa t
	s=$SRC/$(get "$TINYALSA_URL" "$TINYALSA_SHA")
	patch -s -d "$s" -p1 < "$HERE/tinyalsa/0001-sync-ptr-without-mmap.patch"
	mkdir -p "$d" "$SRC/nodl"
	: > "$SRC/nodl/dlfcn.h"	# included even without plugins, and unused
	flat 16384
	for t in tinyplay tinypcminfo tinymix; do
		"$CC" "${CFLAGS[@]}" -I"$s/include" -I"$SRC/nodl" "$s/utils/$t.c" "$s"/src/{pcm,pcm_hw,mixer,mixer_hw,limits}.c \
			"$HERE/tinyalsa/noplugins.c" \
			"${LDFLAT[@]}" -o "$d/$t"
		zero_page "$d/$t.gdb"
		rm -f "$d/$t.gdb"
	done
	cp "$s/NOTICE" "$d/"
	ls -l "$d"
}

# tinyalsa as a library in the prefix, for the programs that play sound
libtinyalsa() {
	local s
	[ -f "$P/lib/libtinyalsa.a" ] && return
	mkdir -p "$P/lib"
	s=$SRC/$(get "$TINYALSA_URL" "$TINYALSA_SHA")
	patch -s -d "$s" -p1 < "$HERE/tinyalsa/0001-sync-ptr-without-mmap.patch"
	mkdir -p "$SRC/nodl" "$P/include/tinyalsa" && : > "$SRC/nodl/dlfcn.h"
	for t in pcm pcm_hw mixer mixer_hw limits; do
		"$CC" $LIBCFLAGS -I"$s/include" -I"$SRC/nodl" -c "$s/src/$t.c" -o "$SRC/nodl/$t.o"
	done
	"$CC" $LIBCFLAGS -c "$HERE/tinyalsa/noplugins.c" -I"$s/include" -o "$SRC/nodl/noplugins.o"
	"$T-ar" rcs "$P/lib/libtinyalsa.a" "$SRC"/nodl/*.o
	cp "$s"/include/tinyalsa/*.h "$P/include/tinyalsa/"
}

# A game's sound (sound/bsound.h): $SRC/bsound.o, to link with
# BSOUND_LIBS; its header is in $HERE/sound
BSOUND_LIBS="-L$P/lib -ltinyalsa -lpthread"
bsound() {
	libtinyalsa
	"$CC" $LIBCFLAGS -Wall -I"$P/include" -c "$HERE/sound/bsound.c" -o "$SRC/bsound.o"
}

# AirPlay 1 to the speaker: Shairport Sync with a tinyalsa back end of our own
# (airplay/audio_tinyalsa.c) instead of alsa-lib, its own mDNS (tinysvcmdns),
# mbedtls from build_deps, and libconfig and popt at Buildroot's versions.
SHAIRPORT_URL=https://github.com/mikebrady/shairport-sync/archive/4.3.7/shairport-sync-4.3.7.tar.gz
SHAIRPORT_SHA=a1242d100b61fe1fffbbf706e919ed51d6a341c9fb8293fb42046e32ae2b3338
LIBCONFIG_URL=https://github.com/hyperrealm/libconfig/releases/download/v1.7.3/libconfig-1.7.3.tar.gz
LIBCONFIG_SHA=545166d6cac037744381d1e9cc5a5405094e7bfad16a411699bcff40bbb31ee7
POPT_URL=http://ftp.rpm.org/popt/releases/popt-1.x/popt-1.19.tar.gz
POPT_SHA=c25a4838fc8e4c1c8aacb8bd620edb3084a3d63bf8987fdad3ca2758c63240f9
# raopsend's Ed25519 and X25519 (pairing with a code), which mbedtls lacks:
# Monocypher, small, audited, BSD-2-Clause or CC0
MONOCYPHER_URL=https://monocypher.org/download/monocypher-4.0.2.tar.gz
MONOCYPHER_SHA=38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c

build_airplay() {
	local s d=$DEST/airplay t
	build_deps
	if [ ! -f "$P/lib/libconfig.a" ]; then
		s=$SRC/$(get "$LIBCONFIG_URL" "$LIBCONFIG_SHA")
		# its config.sub predates riscv32
		cp "$BR_OUT"/host/share/automake-*/config.sub "$s/aux-build/"
		(cd "$s" && cfg --disable-cxx --disable-examples --disable-tests && make -s -C lib install)
	fi
	if [ ! -f "$P/lib/libpopt.a" ]; then
		s=$SRC/$(get "$POPT_URL" "$POPT_SHA")
		(cd "$s" && cfg --disable-nls && make -s -C src install && install -m 644 popt.pc "$P/lib/pkgconfig/")
	fi
	libtinyalsa
	s=$SRC/$(get "$SHAIRPORT_URL" "$SHAIRPORT_SHA")
	patch -s -d "$s" -p1 < "$HERE/airplay/0001-tinyalsa-backend-badge.patch"
	cp "$HERE/airplay/audio_tinyalsa.c" "$s/"
	"$CC" $LIBCFLAGS -c "$HERE/airplay/threadstack.c" -o "$SRC/airplay-threadstack.o"
	(cd "$s" && PATH=$BR_OUT/host/bin:$PATH autoreconf -fi >/dev/null 2>&1 && \
		PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG=$BR_OUT/host/bin/pkg-config CC="$CC" \
		CFLAGS="$LIBCFLAGS -I$P/include -w" CPPFLAGS="-I$P/include" \
		LDFLAGS="-static -L$P/lib -Wl,--wrap=pthread_create -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r" \
		LIBS="$SRC/airplay-threadstack.o -ltinyalsa -lpthread" ./configure --host=riscv32-buildroot-linux-uclibc \
		--with-tinyalsa --with-pipe --with-stdout --with-tinysvcmdns --with-ssl=mbedtls \
		--without-alsa --without-avahi --without-libdaemon --without-soxr --without-airplay-2 \
		>/dev/null)
	# automake links with C++ for a C++ source that is not built here
	make -s -C "$s" -j"$(nproc)" CXXLD="$CC"
	"$T-flthdr" -s 131072 "$s/shairport-sync"	# its stack, logging takes 16 KB (configure splits -s N)
	zero_page "$s/shairport-sync.gdb"
	rm -rf "$d"
	mkdir -p "$d"
	cp "$s/shairport-sync" "$d/"
	# and the other way: the badge's sound to an AirPlay speaker
	t=$SRC/$(get "$MONOCYPHER_URL" "$MONOCYPHER_SHA")
	flat 32768
	"$CC" "${CFLAGS[@]}" -DRAOP_CRYPTO -I"$P/include" -I"$t/src" -I"$t/src/optional" "$HERE/airplay/raopsend.c" \
		"$t/src/monocypher.c" "$t/src/optional/monocypher-ed25519.c" -L"$P/lib" \
		-ltinyalsa -lmbedtls -lmbedx509 -lmbedcrypto -lpthread "${LDFLAT[@]}" -o "$d/raopsend"
	zero_page "$d/raopsend.gdb"
	rm -f "$d/raopsend.gdb"
	cp "$s/LICENSES" "$d/LICENSE"
	{ printf '\n\nraopsend uses Monocypher 4.0.2:\n\n'; cat "$t/LICENCE.md"; } >> "$d/LICENSE"
	cp "$HERE/airplay/start" "$HERE/airplay/app.conf" "$HERE/airplay/shairport-sync.conf" "$d/"
	ls -l "$d"
}

# The sensor panel: BME680 and BMI270 readings, live (it was in the image),
# on the SDK.
build_sensorpanel() {
	sdk_app sensorpanel 16384 "$HERE/sensorpanel/sensorpanel.c"
}

# An app on the WhyOS SDK (sdk/README.md): its C files with whyos.h, which
# brings libwb. sdk_app NAME STACK FILE... [-lm]
sdk_app() {
	local name=$1 stack=$2 d=$DEST/$1
	shift 2
	mkdir -p "$d"
	flat "$stack"
	"$CC" "${CFLAGS[@]}" -Wall -I"$HERE/../sdk/include" -I"$HERE/../libwb" "$@" "${LDFLAT[@]}" -o "$d/$name"
	zero_page "$d/$name.gdb"
	rm -f "$d/$name.gdb"
	cp "$HERE/$name/app.conf" "$d/"
	ls -l "$d"
}

# The calculator: the SDK's entry screen around its sums (calc/sums.c),
# libm for the functions.
build_calc() {
	sdk_app calc 16384 "$HERE/calc/calc.c" "$HERE/calc/sums.c" -lm
}

# The flashlight: the SDK's colour screen.
build_flashlight() {
	sdk_app flashlight 16384 "$HERE/flashlight/flashlight.c"
}

# HIDeous (hideous/hideous.c): saved texts typed on a computer through
# usbd's keyboard, and its mouse; drawn as the calculator is.
build_hideous() {
	local d=$DEST/hideous O=$HERE/../patches/buildroot/overlay-src
	mkdir -p "$d"
	flat 16384
	"$CC" "${CFLAGS[@]}" -Wall -I"$O" -I"$HERE/../libwb" "$HERE/hideous/hideous.c" "${LDFLAT[@]}" -o "$d/hideous"
	zero_page "$d/hideous.gdb"
	rm -f "$d/hideous.gdb"
	cp "$HERE/hideous/app.conf" "$d/"
	ls -l "$d"
}

# BARbarious (barbarious/): texts as barcodes and QR Codes for a scanner,
# drawn as HIDeous is; its QR Codes come from overlay-src's qrcodegen
# (NDEBUG: no assert, which needs stdio).
build_barbarious() {
	local d=$DEST/barbarious O=$HERE/../patches/buildroot/overlay-src
	mkdir -p "$d"
	flat 32768
	"$CC" "${CFLAGS[@]}" -Wall -DNDEBUG -I"$O" -I"$HERE/../libwb" "$HERE/barbarious/barbarious.c" "$HERE/barbarious/symbol.c" \
		"$O/qrcodegen/qrcodegen.c" "${LDFLAT[@]}" -o "$d/barbarious"
	zero_page "$d/barbarious.gdb"
	rm -f "$d/barbarious.gdb"
	cp "$HERE/barbarious/app.conf" "$d/"
	ls -l "$d"
}

# OHMinous (ohminous/): the workbench that took over the Bus Pirate app;
# drawn as the menu is, the badge's pins through /dev/gpiochip0.
build_ohminous() {
	local d=$DEST/ohminous O=$HERE/../patches/buildroot/overlay-src
	mkdir -p "$d"
	flat 32768
	"$CC" "${CFLAGS[@]}" -Wall -I"$O" -I"$HERE/../libwb" "$HERE/ohminous/ohminous.c" "$HERE/ohminous/hw.c" "$HERE/ohminous/bp.c" \
		"${LDFLAT[@]}" -o "$d/ohminous"
	zero_page "$d/ohminous.gdb"
	rm -f "$d/ohminous.gdb"
	cp "$HERE/ohminous/app.conf" "$d/"
	ls -l "$d"
}

# The PDF viewer (pdfview/pdfview.c): MuPDF 1.28.5 renders, cut down to
# PDF (no HTML, XPS, SVG, JavaScript, ICC profiles, JPEG 2000, CJK or Noto
# fonts; the base 14 fonts stay), drawn as imgview is. MuPDF always
# compiles two C++ stubs (OCR, barcodes): its C++ compiler is br-cxx's
# (br.sh cxx), as nmap's. AGPL, as MuPDF.
MUPDF_URL=https://mupdf.com/downloads/archive/mupdf-1.28.5-source.tar.gz
MUPDF_SHA=98a5c10cda20c3992cdf76ff6b2a1149c32bd79cc796d3f703230b1185b7e934
MUPDF_OPTS=(HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no html=no xps=no svg=no extract=no mujs=no brotli=no
	tofu=yes tofu_cjk=yes HAVE_OBJCOPY=no)	# no objcopy: the fonts as C arrays, not "ld -b binary" objects
MUPDF_DEFS="-DFZ_ENABLE_ICC=0 -DFZ_ENABLE_JPX=0 -DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0 -DTOFU_SIL \
	-DFZ_ENABLE_HYPHEN=0 -DFZ_PLOTTERS_CMYK=0 -DFZ_PLOTTERS_N=0 -DFZ_ENABLE_OCR_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0"

build_pdfview() {
	local s d=$DEST/pdfview O=$HERE/../patches/buildroot/overlay-src X=$CXX_OUT/host/bin/riscv32-buildroot-linux-uclibc
	[ -x "$X-g++" ] || { echo "no C++ toolchain: see WhyOS BUILDING.md" >&2; exit 1; }
	s=$SRC/$(get "$MUPDF_URL" "$MUPDF_SHA")
	make -s -C "$s" -j"$(nproc)" libs build=release OS=Linux OUT=build/badge CC="$CC" CXX="$X-g++" \
		AR="$T-ar" "${MUPDF_OPTS[@]}" XCFLAGS="$LIBCFLAGS $MUPDF_DEFS"
	mkdir -p "$d"
	flat 262144
	"$CC" "${CFLAGS[@]}" -Wall -I"$O" -I"$HERE/../libwb" -I"$s/include" "$HERE/pdfview/pdfview.c" \
		"$s/build/badge/libmupdf.a" "$s/build/badge/libmupdf-third.a" -lm "${LDFLAT[@]}" -o "$d/pdfview"
	zero_page "$d/pdfview.gdb"
	rm -f "$d/pdfview.gdb"
	cp "$HERE/pdfview/app.conf" "$d/"
	ls -l "$d"
}

# DOOM: fbDOOM (no SDL, the framebuffer and the VT's keys) with our patches
# (global-patches/fbdoom), its sound effects through bsound (doom/i_badgesound.c,
# doom/0001-badge-sound.patch), the shareware WAD, and doom.c to start it.
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
	for p in "$HERE"/../patches/buildroot/global-patches/fbdoom/*.patch "$HERE/doom/0001-badge-sound.patch"; do
		patch -s -d "$s" -p1 < "$p"
	done
	cp "$HERE/doom/i_badgesound.c" "$s/fbdoom/"
	bsound
	# its Makefile takes link flags only through CC, where "-s N" would
	# split: the stack (DOOM recurses; 64 KB as the package had) via flthdr
	make -s -C "$s/fbdoom" CC="$CC ${CFLAGS[*]} -Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r" \
		NOSDL=1 BADGE_SOUND=1 BADGE_SOUND_CFLAGS="-I$HERE/sound" \
		BADGE_SOUND_LIBS="$SRC/bsound.o $BSOUND_LIBS" fbdoom
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

# OpenTyrian with our SDL for the panel, keypad and sound (opentyrian/sdl, bsound),
# and the freeware Tyrian 2.1 data without its DOS programs.
OPENTYRIAN_VER=2.1.20260913
OPENTYRIAN_SHA=dbcd96383d4fa571137242c36bd7eca054cf5a08a9bf2eec15ef230d6e60680d
TYRIAN_URL=https://www.camanis.net/tyrian/tyrian21.zip
TYRIAN_SHA=7790d09a2a3addcd33c66ef063d5900eb81cc9c342f4807eb8356364dd1d9277

build_opentyrian() {
	local d=$DEST/opentyrian s=$SRC/opentyrian-$OPENTYRIAN_VER f=$SRC/dl/opentyrian-$OPENTYRIAN_VER.tar.gz w=$SRC/dl/tyrian21.zip
	mkdir -p "$SRC/dl" "$d"
	[ -f "$f" ] || curl -sSfL -o "$f" "https://github.com/opentyrian/opentyrian/archive/refs/tags/v$OPENTYRIAN_VER.tar.gz"
	echo "$OPENTYRIAN_SHA  $f" | sha256sum -c --quiet -
	[ -f "$w" ] || curl -sSfL -o "$w" "$TYRIAN_URL"
	echo "$TYRIAN_SHA  $w" | sha256sum -c --quiet -
	rm -rf "$s"
	tar -C "$SRC" -xzf "$f"
	for p in "$HERE"/opentyrian/patches/*.patch; do
		patch -s -d "$s" -p1 < "$p"
	done
	cp "$HERE/opentyrian/sdl/sdl_badge.c" "$s/src/"
	bsound
	make -s -j"$(nproc)" -C "$s" CC="$CC" PKG_CONFIG=false PLATFORM=UNIX WITH_NETWORK=false TYRIAN_DIR=/nonexistent \
		VCS_IDREV="echo v$OPENTYRIAN_VER" CFLAGS="${CFLAGS[*]}" \
		CPPFLAGS="-D_GNU_SOURCE -I$HERE/opentyrian/sdl -I$HERE/sound -I$HERE/../libwb" \
		LDFLAGS="-Wl,--gc-sections -Wl,--no-relax -Wl,-elf2flt=-r" LDLIBS="$SRC/bsound.o $BSOUND_LIBS"
	zero_page "$s/opentyrian.gdb"
	"$T-flthdr" -s 131072 "$s/opentyrian"	# JE_loadMap alone takes 75 KB
	cp "$s/opentyrian" "$d/"
	rm -rf "$d/data" "$SRC/tyrian21"
	unzip -q -o -d "$SRC" "$w"
	mkdir -p "$d/data"
	find "$SRC/tyrian21" -type f ! -iname '*.exe' -exec cp {} "$d/data/" \;
	cp "$HERE/opentyrian/app.conf" "$d/"
	ls -l "$d"
}

# Duke Nukem 3D: the port to the ESP32-P4 in RetroESP32-P4 (Chocolate Duke3D with its
# SDL compat headers and the game's data), with our SDL for the panel, keypad and sound
# (duke3d/sdl, on bsound). Only those paths of that repository are fetched.
DUKE3D_REPO=https://github.com/giltal/RetroESP32-P4
DUKE3D_REF=339f17ff74eea4fe250d749fdcf0b5e544519c2a
DUKE3D_GRP_SHA=89a60aa26333bd659a9aa1aff77666e061da64317349a6a742c1d92754836f2c

duke3d_source() {
	local r=$SRC/dl/RetroESP32-P4-$DUKE3D_REF s=$SRC/duke3d
	mkdir -p "$SRC/dl"
	if [ ! -d "$r/.git" ]; then
		git init -q "$r"
		git -C "$r" remote add origin "$DUKE3D_REPO"
		git -C "$r" sparse-checkout set components/duke3d apps/psram_duke3d/compat SDcard/roms/duke3d
		git -C "$r" fetch -q --depth 1 --filter=blob:none origin "$DUKE3D_REF"
		git -C "$r" checkout -q FETCH_HEAD
	fi
	echo "$DUKE3D_GRP_SHA  $r/SDcard/roms/duke3d/duke3d.grp" | sha256sum -c --quiet -
	rm -rf "$s"
	mkdir -p "$s"
	cp -R "$r/components/duke3d/Engine" "$r/components/duke3d/Game" "$r/components/duke3d/audiolib" "$s/"
	cp -R "$r/apps/psram_duke3d/compat" "$s/"
	for p in "$HERE"/duke3d/patches/*.patch; do
		patch -s -d "$s" -p1 < "$p"
	done
}

build_duke3d() {
	local d=$DEST/duke3d s=$SRC/duke3d o=$SRC/duke3d/obj f flags=() objs=()
	duke3d_source
	mkdir -p "$d" "$o"
	while read -r -a line; do flags+=("${line[@]}"); done < "$HERE/duke3d/flags"
	flags+=(-O2 -fPIC -static -ffunction-sections -fdata-sections -w "-I$HERE/duke3d/sdl" "-I$HERE/../libwb"
		"-I$s/compat" "-I$s/Engine" "-I$s/Game" "-I$s/audiolib")
	for f in $(cat "$HERE/duke3d/sources") ; do
		"$CC" "${flags[@]}" -c "$s/$f" -o "$o/$(basename "$f").o" &
		objs+=("$o/$(basename "$f").o")
		[ "$(jobs -r | wc -l)" -lt "$(nproc)" ] || wait -n
	done
	wait
	bsound
	"$CC" "${flags[@]}" "-I$HERE/sound" -c "$HERE/duke3d/sdl/sdl_badge.c" -o "$o/sdl_badge.o"
	flat 262144
	"$CC" -static "${objs[@]}" "$o/sdl_badge.o" "$SRC/bsound.o" "${LDFLAT[@]}" $BSOUND_LIBS -o "$d/duke3d"
	zero_page "$d/duke3d.gdb"
	rm -f "$d/duke3d.gdb"
	cp "$SRC/dl/RetroESP32-P4-$DUKE3D_REF/SDcard/roms/duke3d/duke3d.grp" "$d/"
	cp "$HERE/duke3d/app.conf" "$d/"
	ls -l "$d"
}

# Commander Keen 4: Omnispeak, the reimplementation of its engine, with the panel, keypad
# and clock of keen4/badge.c, its AdLib and PC speaker through bsound (keen4/sound.c), and
# the shareware data its site offers, without the DOS game.
OMNISPEAK_REF=46cefb33b5f604448da9b81d67368aed82b4c3da
OMNISPEAK_SHA=b74bf6e46b1aa08fb14ea3d81760ab1a8e06688b6e07febccdb5f04e5ee2258c
KEEN4_URL=https://davidgow.net/keen/4keen14.zip
KEEN4_SHA=f92e2d02d3779a2e717a50287ef3ed116c22fdf7bcc12ccf676a412e97195cfe

keen4_source() {
	local s=$SRC/omnispeak-$OMNISPEAK_REF f=$SRC/dl/omnispeak-$OMNISPEAK_REF.tar.gz w=$SRC/dl/4keen14.zip
	mkdir -p "$SRC/dl"
	[ -f "$f" ] || curl -sSfL -o "$f" "https://github.com/sulix/omnispeak/archive/$OMNISPEAK_REF.tar.gz"
	echo "$OMNISPEAK_SHA  $f" | sha256sum -c --quiet -
	[ -f "$w" ] || curl -sSfL -o "$w" "$KEEN4_URL"
	echo "$KEEN4_SHA  $w" | sha256sum -c --quiet -
	rm -rf "$s"
	tar -C "$SRC" -xzf "$f"
	for p in "$HERE"/keen4/patches/*.patch; do
		patch -s -d "$s" -p1 < "$p"
	done
}

build_keen4() {
	local d=$DEST/keen4 s=$SRC/omnispeak-$OMNISPEAK_REF o=$SRC/omnispeak-$OMNISPEAK_REF/obj f objs=()
	local flags=(-std=gnu99 -O2 -fPIC -static -ffunction-sections -fdata-sections -w -DWITH_KEEN4 -DWHYOS
		"-I$s/src" "-I$HERE/../libwb")
	keen4_source
	mkdir -p "$d" "$o/opl"
	for f in $(cat "$HERE/keen4/sources") ; do
		"$CC" "${flags[@]}" -c "$s/src/$f" -o "$o/$f.o" &
		objs+=("$o/$f.o")
		[ "$(jobs -r | wc -l)" -lt "$(nproc)" ] || wait -n
	done
	wait
	bsound
	"$CC" "${flags[@]}" -c "$HERE/keen4/badge.c" -o "$o/badge.o"
	"$CC" "${flags[@]}" "-I$HERE/sound" -c "$HERE/keen4/sound.c" -o "$o/sound.o"
	flat 65536
	"$CC" -static "${objs[@]}" "$o/badge.o" "$o/sound.o" "$SRC/bsound.o" "${LDFLAT[@]}" $BSOUND_LIBS -lm -o "$d/keen4"
	zero_page "$d/keen4.gdb"
	rm -f "$d/keen4.gdb"
	unzip -q -o -d "$d" "$SRC/dl/4keen14.zip" -x '*.exe'
	cp "$s/data/keen4/"* "$d/"
	cp "$s/LICENSE" "$d/LICENSE-omnispeak"
	cp "$HERE/keen4/app.conf" "$d/"
	ls -l "$d"
}

# MicroPython 1.29.0, the Unix port: the REPL in appterm's window and the
# micropython command (micropython/start). Its own readline, mbedTLS for
# ssl (mip installs over HTTPS) and the RV32 native emitter; no threads,
# ffi, btree or littlefs/FAT VFS. mpy-cross runs on the build VM to freeze
# the bundled modules (mip, asyncio, ...) and meshcore_py, the MeshCore
# companion library (micropython/meshcore-py, staged by its stage.sh), and
# our own modules (micropython/modules: setting, the badge's settings;
# ics, calendars).
# micropython/*.patch: int.from_bytes takes signed=, which meshcore_py
# uses; getaddrinfo() asks only for the address families the badge has
# (AI_ADDRCONFIG, as glibc), so a site with an IPv6 address is reached
# over IPv4 (the badge has no IPv6). Its printf and uClibc's both define vsnprintf: MicroPython's wins
# (it ties output to its streams), uClibc keeps its own for internal calls.
MPY_URL=https://github.com/micropython/micropython/releases/download/v1.29.0/micropython-1.29.0.tar.xz
MPY_SHA=d925a7c664e79a2bdf3dfcb285ba5e2237041cc35a0bd4ee573b6c5711efeca0

build_micropython() {
	local s d=$DEST/micropython
	s=$SRC/$(get "$MPY_URL" "$MPY_SHA")
	for f in "$HERE"/micropython/*.patch; do	# get unpacks afresh: apply each
		patch -d "$s" -p1 -s < "$f"
	done
	"$HERE/micropython/meshcore-py/stage.sh" "$SRC/meshcore-py"
	cat > "$SRC/meshcore-py/manifest.py" <<-EOF
	include("\$(PORT_DIR)/variants/standard/manifest.py")
	package("meshcore")
	package("cayennelpp")
	module("setting.py", base_path="$HERE/micropython/modules")
	module("ics.py", base_path="$HERE/micropython/modules")
	package("xml", base_path="$HERE/micropython/modules")
	EOF
	# from scratch: after a change, its incremental build fails in
	# makeqstrdefs.py (compressed.split)
	rm -rf "$s/ports/unix/build-standard"
	make -s -C "$s/mpy-cross" -j"$(nproc)"
	make -s -C "$s/ports/unix" -j"$(nproc)" CROSS_COMPILE="$T-" STRIP=true SIZE=true CWARN=-Wall \
		FROZEN_MANIFEST="$SRC/meshcore-py/manifest.py" USER_C_MODULES="$HERE/../sdk/python" \
		MICROPY_PY_FFI=0 MICROPY_PY_BTREE=0 MICROPY_PY_THREAD=0 \
		MICROPY_VFS_LFS1=0 MICROPY_VFS_LFS2=0 MICROPY_VFS_FAT=0 CFLAGS_EXTRA="-fPIC" \
		LDFLAGS_EXTRA="-static -Wl,--allow-multiple-definition -Wl,--no-relax -Wl,-elf2flt=-r"
	zero_page "$s/ports/unix/build-standard/micropython.gdb"
	# its stack: main.c limits Python's recursion at 40000 bytes
	"$T-flthdr" -s 65536 "$s/ports/unix/build-standard/micropython"
	rm -rf "$d"
	mkdir -p "$d"
	cp "$s/ports/unix/build-standard/micropython" "$s/LICENSE" "$d/"
	cp "$HERE/micropython/start" "$HERE/micropython/app.conf" "$d/"
	cp "$SRC/meshcore-py/meshcore/LICENSE" "$d/LICENSE-meshcore_py"
	cp "$SRC/meshcore-py/cayennelpp/LICENSE" "$d/LICENSE-pycayennelpp"
	mkdir -p "$d/examples"
	cp "$HERE/micropython/meshcore-py/examples/ping_bot.py" "$d/examples/"
	ls -l "$d"
}

# The VNC server (vnc/vncd.c): the screen and the keypad over the
# network, a service that runs as root beside the apps (SERVICE in
# app.conf); its DES is mbedtls's. The app itself is vnc/info.
build_vnc() {
	local d=$DEST/vnc
	build_deps
	rm -rf "$d"
	mkdir -p "$d"
	flat 32768
	"$CC" "${CFLAGS[@]}" -I"$P/include" "$HERE/vnc/vncd.c" -L"$P/lib" -lmbedcrypto -lz \
		"${LDFLAT[@]}" -o "$d/vncd"
	zero_page "$d/vncd.gdb"
	rm -f "$d/vncd.gdb"
	cp "$HERE/vnc/app.conf" "$d/"
	ls -l "$d"
}

for app in "${@:-gnuboy netsurf nmap imgview links nethack doom sensorpanel nano calc flashlight hideous barbarious pdfview ohminous duke3d opentyrian keen4 micropython vnc}"; do
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
		nano) build_nano ;;
		calc) build_calc ;;
		flashlight) build_flashlight ;;
		hideous) build_hideous ;;
		barbarious) build_barbarious ;;
		pdfview) build_pdfview ;;
		ohminous) build_ohminous ;;
		opentyrian) build_opentyrian ;;
		duke3d) build_duke3d ;;
		keen4) build_keen4 ;;
		micropython) build_micropython ;;
		tinyalsa) build_tinyalsa ;;
		airplay) build_airplay ;;
		vnc) build_vnc ;;
		*) echo "unknown app $a" >&2; exit 2 ;;
		esac
	done
done
