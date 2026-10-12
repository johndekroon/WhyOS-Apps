# The store client (docs/client.md): host build for the tests and CI.
#   make -C client host        client/store-host, against a host mbedTLS (deps.sh host)
#   make -C client ui-host     client/store-ui-host: the screens, keys from a
#                              script, screens as PPM (tests/test_ui.py)
#   make -C client test        the client's tests (tests/, pytest)
# The badge build is build.sh. The screens draw with the menu's code from
# why2025-linux (WHY2025_LINUX, default ../../why2025-linux).
DEPS    = .deps/host
CC     ?= cc
# bounded snprintf truncates on purpose: no format-truncation warnings
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -Wno-format-truncation -fsanitize=address,undefined
SRCS    = $(filter-out src/ui.c,$(wildcard src/*.c))
HDRS    = $(wildcard src/*.h) src/roots.inc
WHY2025_LINUX ?= $(abspath ../../why2025-linux)
MENU    = $(WHY2025_LINUX)/patches/buildroot/overlay-src

host: store-host

store-host: $(SRCS) $(HDRS) $(DEPS)/lib/libmbedtls.a
	$(CC) $(CFLAGS) -DSTORE_HOST -I$(DEPS)/include -Isrc -o $@ $(SRCS) \
		-L$(DEPS)/lib -lmbedtls -lmbedx509 -lmbedcrypto -lz

ui-host: store-ui-host

store-ui-host: $(wildcard src/*.c) $(HDRS) $(DEPS)/lib/libmbedtls.a $(MENU)/wbdraw.h $(MENU)/home_assets.h
	$(CC) $(CFLAGS) -Wno-sign-compare -DSTORE_UI -DSTORE_UI_HOST -I$(DEPS)/include -Isrc -I$(MENU) \
		-o $@ $(wildcard src/*.c) -L$(DEPS)/lib -lmbedtls -lmbedx509 -lmbedcrypto -lz

# the roots as a C string (net.c)
src/roots.inc: roots.pem
	sed -e '/^#/d' -e 's/.*/"&\\n"/' roots.pem > $@

$(DEPS)/lib/libmbedtls.a:
	./deps.sh host

# the screens' test only where the menu's drawing code is (not in CI)
UI_HOST = $(if $(wildcard $(MENU)/wbdraw.h),store-ui-host)

test: store-host $(UI_HOST)
	python3 -m pytest -q tests

clean:
	rm -rf store-host store-ui-host store-host.dSYM src/roots.inc

.PHONY: host ui-host test clean
