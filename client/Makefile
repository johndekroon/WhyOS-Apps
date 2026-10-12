# The store client (docs/client.md): host build for the tests and CI.
#   make -C client host        client/store-host, against a host mbedTLS (deps.sh host)
#   make -C client test        the client's tests (tests/, pytest)
# The badge build is build.sh.
DEPS    = .deps/host
CC     ?= cc
# bounded snprintf truncates on purpose: no format-truncation warnings
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -Wno-format-truncation -fsanitize=address,undefined
SRCS    = $(filter-out src/ui.c,$(wildcard src/*.c))
HDRS    = $(wildcard src/*.h) src/roots.inc

host: store-host

store-host: $(SRCS) $(HDRS) $(DEPS)/lib/libmbedtls.a
	$(CC) $(CFLAGS) -DSTORE_HOST -I$(DEPS)/include -Isrc -o $@ $(SRCS) \
		-L$(DEPS)/lib -lmbedtls -lmbedx509 -lmbedcrypto -lz

# the roots as a C string (net.c)
src/roots.inc: roots.pem
	sed -e '/^#/d' -e 's/.*/"&\\n"/' roots.pem > $@

$(DEPS)/lib/libmbedtls.a:
	./deps.sh host

test: store-host
	python3 -m pytest -q tests

clean:
	rm -rf store-host store-host.dSYM src/roots.inc

.PHONY: host test clean
