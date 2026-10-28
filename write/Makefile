# Write, the WhyOS text editor: host builds for the tests.
#   make -C write test        the modules' unit tests (tests/unit), then the
#                             screens (tests/test_ui.py) where libwb's
#                             drawing code is
#   make -C write ui-host     write-ui-host-<system>: the screens, keys from a script,
#                             screens as PPM
# The badge build is build.sh. The screens draw with libwb from
# why2025-linux (WHY2025_LINUX, default ../../why2025-linux; libwb/).
CC     ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -Wno-format-truncation -fsanitize=address,undefined
WHY2025_LINUX ?= $(abspath ../../why2025-linux)
MENU    = $(WHY2025_LINUX)/libwb

# the modules without the screens
CORE    = $(filter-out src/ui.c src/main.c,$(wildcard src/*.c))
HDRS    = $(wildcard src/*.h)
# one folder per system: this folder is shared with the build VM
BIN     = tests/unit/bin-$(shell uname -s)
UNITS   = $(patsubst tests/unit/%.c,$(BIN)/%,$(wildcard tests/unit/test_*.c))

units: $(UNITS)

$(BIN)/%: tests/unit/%.c tests/unit/t.h $(CORE) $(HDRS)
	@mkdir -p $(BIN)
	$(CC) $(CFLAGS) -DWRITE_HOST -Isrc -Itests/unit -o $@ $< $(CORE)

UI_BIN  = write-ui-host-$(shell uname -s)

ui-host: $(UI_BIN)

$(UI_BIN): $(wildcard src/*.c) $(HDRS) $(MENU)/wbdraw.h $(MENU)/wb_assets.h
	$(CC) $(CFLAGS) -Wno-sign-compare -DWRITE_HOST -DWRITE_UI_HOST -DWB_HOST -Isrc -I$(MENU) \
		-o $@ $(wildcard src/*.c)

UI_HOST = $(if $(wildcard $(MENU)/wbdraw.h),$(UI_BIN))

unit-test: units
	@set -e; for t in $(UNITS); do $$t; done

test: unit-test $(UI_HOST)
	$(if $(UI_HOST),python3 -m pytest -q tests)

clean:
	rm -rf tests/unit/bin-* write-ui-host-* *.dSYM

.PHONY: units unit-test ui-host test clean
