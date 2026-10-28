# Don't Look Behind You: build, tests, sanitizers, boundary check.
# `make asan` re-runs this Makefile with BUILD=build/asan and sanitizer flags.

CC       ?= gcc
AR       ?= ar
CPPFLAGS := -Isrc/common -D_GNU_SOURCE
CFLAGS   := -std=c17 -Wall -Wextra -Werror -Wvla -Wpedantic -O2 -g
ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer

BUILD        ?= build
EXTRA_CFLAGS ?=
ALL_CFLAGS    = $(CFLAGS) $(EXTRA_CFLAGS)

COMMON_HDR := $(wildcard src/common/*.h)
COMMON_SRC := $(wildcard src/common/*.c)
COMMON_OBJ := $(COMMON_SRC:src/common/%.c=$(BUILD)/common/%.o)
LIB        := $(BUILD)/libdlby_common.a

HUB_HDR := $(wildcard src/hub/*.h)
HUB_LIB := $(BUILD)/libdlby_hub.a

STORE_HDR := $(wildcard src/store/*.h)
STORE_LIB := $(BUILD)/libdlby_store.a

SUPER_HDR := $(wildcard src/super/*.h)
SUPER_LIB := $(BUILD)/libdlby_super.a

CAP_HDR := $(wildcard src/capture/*.h)
CAP_LIB := $(BUILD)/libdlby_capture.a

APP_HDR  := $(COMMON_HDR) $(HUB_HDR) $(STORE_HDR) $(SUPER_HDR) $(CAP_HDR)
APP_INC  := -Isrc/hub -Isrc/store -Isrc/super -Isrc/capture
APP_LIBS := $(HUB_LIB) $(STORE_LIB) $(SUPER_LIB) $(CAP_LIB) $(LIB)

# Deployed to the Pi by `make install`.
PRODUCT := $(BUILD)/dlby-super $(BUILD)/store $(BUILD)/fuse-hub $(BUILD)/cap-radar \
	$(BUILD)/cap-sonar

BINS := $(PRODUCT) $(BUILD)/tools/fake-sensors $(BUILD)/tools/fake-child \
	$(BUILD)/tools/dlby-logcat $(BUILD)/tools/fake-ld2451

TEST_SRC := $(wildcard tests/test_*.c)
TESTS    := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%)

PREFIX     ?= /opt/dlby
REMOTE_DIR ?= dlby-src

.PHONY: all test asan check-boundary run-tests clean install deploy

all: $(LIB) $(BINS) $(TESTS)

$(BUILD)/common/%.o: src/common/%.c $(COMMON_HDR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -c -o $@ $<

$(LIB): $(COMMON_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/hub/%.o: src/hub/%.c $(HUB_HDR) $(COMMON_HDR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/hub $(ALL_CFLAGS) -c -o $@ $<

# fuse-hub's testable parts (everything but main), linked into the unit tests.
$(HUB_LIB): $(BUILD)/hub/health.o
	$(AR) rcs $@ $^

$(BUILD)/fuse-hub: $(BUILD)/hub/fuse-hub.o $(HUB_LIB) $(LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $^

# Objects under obj/store/: $(BUILD)/store is the binary.
$(BUILD)/obj/store/%.o: src/store/%.c $(STORE_HDR) $(COMMON_HDR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/store $(ALL_CFLAGS) -c -o $@ $<

# store's testable parts (everything but main), linked into the tests and tools.
$(STORE_LIB): $(BUILD)/obj/store/record.o $(BUILD)/obj/store/seglog.o
	$(AR) rcs $@ $^

$(BUILD)/store: $(BUILD)/obj/store/store.o $(STORE_LIB) $(LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $^

$(BUILD)/obj/super/%.o: src/super/%.c $(SUPER_HDR) $(COMMON_HDR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/super $(ALL_CFLAGS) -c -o $@ $<

# dlby-super's testable parts (everything but main), linked into the unit tests.
$(SUPER_LIB): $(BUILD)/obj/super/supervise.o $(BUILD)/obj/super/notify.o
	$(AR) rcs $@ $^

$(BUILD)/dlby-super: $(BUILD)/obj/super/dlby-super.o $(SUPER_LIB) $(LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $^

# cap-* include health.h for the shared sensor timing constants.
$(BUILD)/obj/capture/%.o: src/capture/%.c $(CAP_HDR) $(HUB_HDR) $(COMMON_HDR)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Isrc/capture -Isrc/hub $(ALL_CFLAGS) -c -o $@ $<

# The capture processes' testable parts (everything but main), linked into the unit tests.
$(CAP_LIB): $(BUILD)/obj/capture/radar_parse.o $(BUILD)/obj/capture/echo.o
	$(AR) rcs $@ $^

$(BUILD)/cap-radar: $(BUILD)/obj/capture/cap-radar.o $(CAP_LIB) $(LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $^

$(BUILD)/cap-sonar: $(BUILD)/obj/capture/cap-sonar.o $(CAP_LIB) $(LIB)
	$(CC) $(ALL_CFLAGS) -o $@ $^

$(BUILD)/tools/%: tools/%.c $(APP_HDR) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(APP_INC) $(ALL_CFLAGS) -o $@ $< $(APP_LIBS)

$(BUILD)/tests/%: tests/%.c tests/check.h $(APP_HDR) $(APP_LIBS)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(APP_INC) -Itests $(ALL_CFLAGS) -o $@ $< $(APP_LIBS)

run-tests: $(TESTS) $(BINS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done
	@echo "== tests/hub_e2e.sh"; sh tests/hub_e2e.sh $(BUILD)
	@echo "== tests/store_e2e.sh"; sh tests/store_e2e.sh $(BUILD)
	@echo "== tests/super_e2e.sh"; sh tests/super_e2e.sh $(BUILD)
	@echo "== tests/capture_e2e.sh"; sh tests/capture_e2e.sh $(BUILD)
	@echo "all tests passed ($(BUILD))"

test: check-boundary run-tests

asan:
	$(MAKE) BUILD=build/asan EXTRA_CFLAGS="$(ASAN_FLAGS)" run-tests

# DESIGN.md §8: the product makes no network calls. Each fixture must be caught,
# so a broken checker can't pass silently.
check-boundary:
	@sh tools/check-boundary.sh src
	@for f in tests/fixtures/boundary_*.c; do \
		if sh tools/check-boundary.sh $$f >/dev/null; then \
			echo "check-boundary: checker missed $$f"; exit 1; \
		fi; \
	done
	@echo "check-boundary: ok"

install: $(PRODUCT)
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)/etc/systemd/system \
		$(DESTDIR)/etc/systemd/system.conf.d $(DESTDIR)/etc/systemd/journald.conf.d
	install -m 0755 $(PRODUCT) $(DESTDIR)$(PREFIX)/bin/
	ln -sf cap-radar $(DESTDIR)$(PREFIX)/bin/cap-radar-left
	install -m 0644 deploy/dlby.service $(DESTDIR)/etc/systemd/system/dlby.service
	install -m 0644 deploy/system.conf.d/dlby-watchdog.conf $(DESTDIR)/etc/systemd/system.conf.d/
	install -m 0644 deploy/journald.conf.d/dlby.conf $(DESTDIR)/etc/systemd/journald.conf.d/

# Builds natively on the Pi (no cross toolchain), installs, and restarts the
# service. It changes a real device, so it is never run by the test targets.
deploy:
	@test -n "$(PI)" || { echo "usage: make deploy PI=pi@<host>"; exit 64; }
	rsync -a --delete --exclude build --exclude .git --exclude .idea ./ $(PI):$(REMOTE_DIR)/
	ssh $(PI) 'set -e; cd $(REMOTE_DIR); make; sudo make install; \
		sudo systemd-analyze verify /etc/systemd/system/dlby.service; \
		sudo systemctl daemon-reexec; sudo systemctl restart systemd-journald; \
		sudo systemctl enable dlby; sudo systemctl restart dlby; \
		systemctl --no-pager status dlby'

clean:
	rm -rf build
