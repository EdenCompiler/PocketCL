# Keep build recipes POSIX: the compiler, make, shell and awk are sufficient.
.POSIX:

CC = cc
BUILD = build/default
CFLAGS = -O2 -g
WARNINGS = -std=c99 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS = -Ikernel/include -Itests/c
LDFLAGS =
LDLIBS = -lm

KERNEL_SOURCES = kernel/src/value.c kernel/src/heap.c kernel/src/package.c kernel/src/reader0.c kernel/src/debugprint.c kernel/src/float_io.c
EVAL_SOURCES = kernel/src/eval0.c kernel/src/prim.c kernel/src/host.c kernel/src/boot.c
HEADERS = kernel/include/pocket.h kernel/include/heap_internal.h kernel/include/float_io.h kernel/include/eval.h kernel/include/host.h kernel/include/boot.h tests/c/test.h
TEST_SOURCES = tests/c/test_main.c tests/c/test_memory.c tests/c/test_package.c tests/c/test_reader.c tests/c/test_eval.c tests/c/test_prim.c
PORTABLE_TESTS = tests/conformance/portable-harness.lisp tests/conformance/core-test.lisp tests/conformance/package-test.lisp

all: $(BUILD)/tests stage0

stage0: $(BUILD)/pocketvm0

$(BUILD)/pocketvm0: kernel/src/main0.c $(KERNEL_SOURCES) $(EVAL_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -DPOCKET_SOURCE_DIR='"$(CURDIR)"' $(WARNINGS) $(CFLAGS) kernel/src/main0.c $(KERNEL_SOURCES) $(EVAL_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/fuzz-reader: tests/c/fuzz_reader.c $(KERNEL_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) tests/c/fuzz_reader.c $(KERNEL_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/tests: $(TEST_SOURCES) $(KERNEL_SOURCES) $(EVAL_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -DPOCKET_TEST_DIR='"$(BUILD)"' $(WARNINGS) $(CFLAGS) $(TEST_SOURCES) $(KERNEL_SOURCES) $(EVAL_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

test: $(BUILD)/tests conformance test-lisp
	$(BUILD)/tests

test-lisp: stage0
	sh tools/check-runtime-conformance.sh $(BUILD)/pocketvm0
	$(BUILD)/pocketvm0 $(PORTABLE_TESTS)

diff-sbcl: test-lisp
	@if command -v sbcl >/dev/null 2>&1; then sbcl --noinform --non-interactive --load tests/conformance/portable-harness.lisp --load tests/conformance/core-test.lisp --load tests/conformance/package-test.lisp; else echo 'SBCL not installed; optional differential oracle unavailable.'; fi

conformance:
	sh tools/check-conformance.sh

# Separate output directories prevent sanitizer/compiler builds reusing binaries.
gcc:
	$(MAKE) test CC=gcc BUILD=build/gcc

clang:
	$(MAKE) test CC=clang BUILD=build/clang

asan:
	$(MAKE) test fuzz CC=clang BUILD=build/sanitize CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'

fuzz: $(BUILD)/fuzz-reader
	$(BUILD)/fuzz-reader 1000000

stress:
	$(MAKE) test BUILD=build/stress CPPFLAGS='-Ikernel/include -Itests/c -DPOCKET_GC_STRESS=1'

ci: gcc clang asan stress

clean:
	rm -rf build

.PHONY: all stage0 test test-lisp diff-sbcl conformance gcc clang asan stress ci clean fuzz
