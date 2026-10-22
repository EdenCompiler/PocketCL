# Performance measurements

**Status: initial stage-0 measurements only.** The VM and full performance suite
remain later milestone work. Do not treat the roadmap's targets as achieved.

## Baseline environment

- Machine: AMD Ryzen 5 5500U development laptop.
- Compiler, version, build flags, OS, and power profile: record with each measurement once the benchmark harness exists.
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
by `/usr/bin/time` were 14,184, 14,340, and 14,076 KiB. M3 must rerun the same
definition beside the VM benchmark before claiming the required speed ratio.

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
