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
EXEC_SOURCES = kernel/src/exec.c kernel/src/prim.c kernel/src/host.c kernel/src/vm.c kernel/src/loader.c kernel/src/fasl_prim.c kernel/src/compiler_prim.c
EVAL_SOURCES = $(EXEC_SOURCES) kernel/src/eval0.c kernel/src/boot.c kernel/src/driver.c
VM_SOURCES = $(EXEC_SOURCES) kernel/src/driver.c
HEADERS = kernel/include/pocket.h kernel/include/heap_internal.h kernel/include/float_io.h kernel/include/eval.h kernel/include/exec.h kernel/include/vm.h kernel/include/loader.h kernel/include/driver.h kernel/include/compiler_prim.h kernel/include/host.h kernel/include/boot.h tests/c/test.h
TEST_SOURCES = tests/c/test_main.c tests/c/test_memory.c tests/c/test_package.c tests/c/test_reader.c tests/c/test_eval.c tests/c/test_prim.c tests/c/test_vm.c tests/c/test_loader.c tests/c/test_compiler_prim.c
PORTABLE_TESTS = tests/conformance/portable-harness.lisp tests/conformance/core-test.lisp tests/conformance/package-test.lisp tests/conformance/condition-test.lisp
BOOT_LISP = lisp/boot/00-core.lisp lisp/boot/10-lambda-list.lisp lisp/boot/20-cl-macros.lisp lisp/boot/30-conditions.lisp
COMPILER_LISP = lisp/compiler/00-package.lisp lisp/compiler/01-tables.lisp lisp/compiler/10-environment.lisp lisp/compiler/20-expand-analyze.lisp lisp/compiler/30-codegen.lisp lisp/compiler/40-driver.lisp lisp/compiler/50-fasl.lisp
COMPILER_TESTS = tests/lisp/compiler-test.lisp tests/lisp/compiler-edge-test.lisp tests/lisp/fasl-test.lisp

all: $(BUILD)/tests stage0 stage1

stage0: $(BUILD)/pocketvm0

stage1: $(BUILD)/pocketvm $(BUILD)/stage1/compiler.fasl

$(BUILD)/pocketvm: kernel/src/main.c $(KERNEL_SOURCES) $(VM_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -DPOCKET_SOURCE_DIR='"$(CURDIR)"' -DPOCKET_BUILD_DIR='"$(BUILD)"' $(WARNINGS) $(CFLAGS) kernel/src/main.c $(KERNEL_SOURCES) $(VM_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/stage1/compiler.fasl: $(BUILD)/pocketvm0 $(BOOT_LISP) $(COMPILER_LISP)
	mkdir -p $(BUILD)/stage1
	$(BUILD)/pocketvm0 --build-image $@

$(BUILD)/stage2/compiler.fasl: $(BUILD)/pocketvm $(BUILD)/stage1/compiler.fasl $(BOOT_LISP) $(COMPILER_LISP)
	mkdir -p $(BUILD)/stage2
	$(BUILD)/pocketvm --build-image $@

bootstrap: $(BUILD)/stage2/compiler.fasl
	cmp $(BUILD)/stage1/compiler.fasl $(BUILD)/stage2/compiler.fasl

benchmark: stage0 stage1
	sh tools/benchmark.sh $(BUILD)/pocketvm0 $(BUILD)/pocketvm $(BUILD)/benchmark

$(BUILD)/pocketvm0: kernel/src/main0.c $(KERNEL_SOURCES) $(EVAL_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -DPOCKET_SOURCE_DIR='"$(CURDIR)"' $(WARNINGS) $(CFLAGS) kernel/src/main0.c $(KERNEL_SOURCES) $(EVAL_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/fuzz-reader: tests/c/fuzz_reader.c $(KERNEL_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) tests/c/fuzz_reader.c $(KERNEL_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/fuzz-fasl: tests/c/fuzz_fasl.c $(KERNEL_SOURCES) $(EXEC_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(CFLAGS) tests/c/fuzz_fasl.c $(KERNEL_SOURCES) $(EXEC_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/tests: $(TEST_SOURCES) $(KERNEL_SOURCES) $(EVAL_SOURCES) $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -DPOCKET_TEST_DIR='"$(BUILD)"' $(WARNINGS) $(CFLAGS) $(TEST_SOURCES) $(KERNEL_SOURCES) $(EVAL_SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

test: $(BUILD)/tests conformance test-lisp test-writer test-compiler test-vm bootstrap
	$(BUILD)/tests

test-lisp: stage0
	sh tools/check-runtime-conformance.sh $(BUILD)/pocketvm0
	$(BUILD)/pocketvm0 $(PORTABLE_TESTS)

# Exercise the PFAS writer/loader boundary without depending on codegen.
test-writer: stage0
	$(BUILD)/pocketvm0 lisp/compiler/00-package.lisp lisp/compiler/01-tables.lisp lisp/compiler/50-fasl.lisp tests/conformance/portable-harness.lisp tests/lisp/writer-test.lisp

# Keep source-compiled checks as well as image-hosted checks: the first tests
# the bootstrap boundary, while the second runs without any eval0 fallback.
test-compiler: stage0
	$(BUILD)/pocketvm0 --vm tests/conformance/portable-harness.lisp $(COMPILER_TESTS)

test-vm: stage1
	sh tools/check-runtime-conformance.sh $(BUILD)/pocketvm
	$(BUILD)/pocketvm $(PORTABLE_TESTS)
	$(BUILD)/pocketvm tests/conformance/portable-harness.lisp $(COMPILER_TESTS) tests/lisp/m3-acceptance.lisp
	sh tools/check-vm-memory.sh $(BUILD)/pocketvm

diff-sbcl: test-lisp
	@if command -v sbcl >/dev/null 2>&1; then sbcl --noinform --non-interactive --load tests/conformance/portable-harness.lisp --load tests/conformance/core-test.lisp --load tests/conformance/package-test.lisp --load tests/conformance/condition-test.lisp; else echo 'SBCL not installed; optional differential oracle unavailable.'; fi

conformance:
	sh tools/check-conformance.sh

# Separate output directories prevent sanitizer/compiler builds reusing binaries.
gcc:
	$(MAKE) test CC=gcc BUILD=build/gcc

clang:
	$(MAKE) test CC=clang BUILD=build/clang

asan:
	$(MAKE) test fuzz CC=clang BUILD=build/sanitize CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined' LDFLAGS='-fsanitize=address,undefined'

fuzz: $(BUILD)/fuzz-reader $(BUILD)/fuzz-fasl
	$(BUILD)/fuzz-reader 1000000
	$(BUILD)/fuzz-fasl 1000000

stress:
	$(MAKE) test BUILD=build/stress CPPFLAGS='-Ikernel/include -Itests/c -DPOCKET_GC_STRESS=1'

ci: gcc clang asan stress

clean:
	rm -rf build

.PHONY: all stage0 stage1 bootstrap benchmark test test-lisp test-writer test-compiler test-vm diff-sbcl conformance gcc clang asan stress ci clean fuzz
