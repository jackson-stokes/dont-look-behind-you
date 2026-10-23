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

APP_HDR  := $(COMMON_HDR) $(HUB_HDR) $(STORE_HDR)
APP_INC  := -Isrc/hub -Isrc/store
APP_LIBS := $(HUB_LIB) $(STORE_LIB) $(LIB)

BINS := $(BUILD)/fuse-hub $(BUILD)/store $(BUILD)/tools/fake-sensors $(BUILD)/tools/dlby-logcat

TEST_SRC := $(wildcard tests/test_*.c)
TESTS    := $(TEST_SRC:tests/%.c=$(BUILD)/tests/%)

.PHONY: all test asan check-boundary run-tests clean

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

clean:
	rm -rf build
