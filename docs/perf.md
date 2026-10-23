# Performance measurements

**Status: M2 and M3 baselines recorded.** The M3 VM benchmark passes its 5×
speedup gate. The full UI performance suite remains later milestone work;
unmeasured roadmap targets are not achieved.

## Baseline environment

- Machine: AMD Ryzen 5 5500U development laptop.
- Compiler, version, build flags, OS, and power profile: record with each measurement.
- Remote CI: GitHub Actions workflows are prepared; remote execution is not connected yet, and macOS results are pending.

## Planned targets

The roadmap's section 12 defines target and hard-gate values for VM Fibonacci, dynamic bindings, condition setup, literal `format`, idle and active frames, layout, list updates, drawlist building, GC pause, startup, image size, and resident memory. M12 will record measurements for those rows and use the specified gate and regression policy. Until then, this page intentionally contains no fabricated numbers.

For reproducible reports, include commit or milestone, machine/OS, compiler and flags, profile (development or runtime), warmup and sample method, measured value, and target/gate. Keep integer replay and raster correctness checks separate from timing results.

## Stage-0 integration observations — 2026-10-21

These measurements use the final M2 implementation. Environment: the reference machine
above, Linux 6.12.111+deb13-amd64 x86_64, GCC 14.2.0, strict C99, `-O2 -g`,
16 MiB development arena (11,184,816 bytes usable heap; the rest is collector
work space). One process per timing sample; startup/boot are included.

The ordinary recursive Fibonacci definition returned 832040 for `(fib 30)` in
14.43, 13.96, and 13.46 seconds elapsed over three sequential fresh-process
samples after correctness checks completed. The median is **13.96 seconds**;
user times were 14.39, 13.94, and 13.38 seconds. Maximum resident sizes reported
by `/usr/bin/time` were 14,184, 14,340, and 14,076 KiB. The M3 comparison below
reruns the same definition beside the VM instead of comparing different dates.

For a direct tail-recursive counter, explicit collection after 1,000 calls and
after 1,000,000 calls in the same process retained exactly **303,416 bytes** in
both cases. This distinguishes bounded live memory from the expected repeated
allocation and collection in the tree interpreter. Reproduce with:

```sh
make stage0
build/default/pocketvm0 \
  -e '(defun memory-tail (n sum) (if (= n 0) sum (memory-tail (1- n) (1+ sum))))' \
  -e '(memory-tail 1000 0)' --stats \
  -e '(memory-tail 1000000 0)' --stats
```

## M3 compiler and VM — 2026-10-23

These are the `m3-done` delivery measurements on the reference Ryzen 5 5500U,
Linux 6.12.111+deb13-amd64 x86_64, GCC 14.2.0, strict C99, `-O2 -g`.
The CPU governor was `powersave` with frequency boost enabled. No other
PocketCL checks ran during these timing samples; ordinary desktop/background
activity was present. This is a laptop baseline, not an isolated CPU result.

`make benchmark` starts three fresh processes per engine, with no warmup.
Elapsed times include source boot or image loading and compilation of the
same ordinary recursive Fibonacci definition. Every sample must return
832040 before the harness compares medians.

| Engine | Sample 1 | Sample 2 | Sample 3 | Median |
| --- | ---: | ---: | ---: | ---: |
| Stage 0 | 14.85 s | 14.91 s | 14.69 s | 14.85 s |
| Native VM | 0.90 s | 0.89 s | 0.95 s | 0.90 s |

The VM is **16.50× faster**, passing M3's minimum 5× gate. Its 0.90-second
median is below the roadmap's 1.0-second Fibonacci gate; the 0.5-second target
is not yet achieved. `make benchmark` currently enforces the M3 ratio. The
remaining absolute budgets and regression policy belong to the M12 suite.

The VM memory check uses one process, calls the same compiled tail counter
1,000 times and 1,000,000 times, and explicitly collects at each `--stats`.
It retains **1,124,992 bytes** at both points in the ordinary build. With GC
stress active it retains **1,125,024 bytes** at both points, with collection
counts increasing from 1,583 to 1,744. The development arena is 16,777,216
bytes: 11,184,816 bytes of heap, with the rest reserved for collector work space.

| Delivery artifact | Bytes | Scope |
| --- | ---: | --- |
| Stage-1 compiler image | 184,060 | Bootstrap library and Lisp compiler |
| Stage-2 compiler image | 184,060 | Byte-identical to stage 1 |
| Native VM executable | 689,760 | Linux development ELF, including debug data |
| Stage-0 executable | 840,352 | Linux development ELF, including debug data |

Compiler images match across GCC, Clang, ASAN/UBSAN and GC-stress builds.
Their fingerprint is recorded in [milestones.md](milestones.md). Executable
sizes above are development artifacts, not stripped application budgets.

For a fresh image load followed by `(+ 20 22)` and `--stats`, `/usr/bin/time -v`
reported 3,616 KiB maximum RSS with the development arena and 3,496 KiB with
`--runtime`. Both retained 1,124,312 heap bytes. The runtime arena is 4,194,304
bytes, including 2,796,208 bytes of heap. These small startup workloads leave
arena pages untouched; RSS is not a claim that the full configured arena is
resident. The 4 MiB configuration still loads the compiler and passes all
11 VM acceptance assertions. Compiler stripping, counter-app startup and
release memory budgets remain later work.

Reproduce the checks with:

```sh
make stage1 bootstrap
make benchmark
sh tools/check-vm-memory.sh build/default/pocketvm
# After make stress has built its host:
sh tools/check-vm-memory.sh build/stress/pocketvm
/usr/bin/time -v build/default/pocketvm -e '(+ 20 22)' --stats
/usr/bin/time -v build/default/pocketvm --runtime -e '(+ 20 22)' --stats
```
