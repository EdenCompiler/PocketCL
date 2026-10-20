# Keep build recipes POSIX: the compiler, make, shell and awk are sufficient.
.POSIX:

CC = cc
BUILD = build/default
CFLAGS = -O2 -g
WARNINGS = -std=c99 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS = -Ikernel/include -Itests/c
LDFLAGS =
LDLIBS = -lm

KERNEL_SOURCES =
TEST_SOURCES = tests/c/test_main.c

all: $(BUILD)/tests

$(BUILD)/tests: $(TEST_SOURCES) $(KERNEL_SOURCES) tests/c/test.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) $(TEST_SOURCES) $(KERNEL_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

test: $(BUILD)/tests conformance
	$(BUILD)/tests

conformance:
	sh tools/check-conformance.sh

# Separate output directories prevent sanitizer/compiler builds reusing binaries.
gcc:
	$(MAKE) test CC=gcc BUILD=build/gcc

clang:
	$(MAKE) test CC=clang BUILD=build/clang

asan:
	$(MAKE) test CC=clang BUILD=build/sanitize CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'

stress:
	$(MAKE) test BUILD=build/stress CPPFLAGS='-Ikernel/include -Itests/c -DPOCKET_GC_STRESS=1'

ci: gcc clang asan stress

clean:
	rm -rf build

.PHONY: all test conformance gcc clang asan stress ci clean
