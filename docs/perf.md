# Performance measurements

**Status: no PocketCL runtime measurements yet.** M0 establishes a reference machine; the performance suite and gates arrive in M12. Do not treat the targets below as achieved results.

## Baseline environment

- Machine: AMD Ryzen 5 5500U development laptop.
- Compiler, version, build flags, OS, and power profile: record with each measurement once the benchmark harness exists.
- Remote CI: GitHub Actions workflows are prepared; remote execution is not connected yet, and macOS results are pending.

## Planned targets

The roadmap's section 12 defines target and hard-gate values for VM Fibonacci, dynamic bindings, condition setup, literal `format`, idle and active frames, layout, list updates, drawlist building, GC pause, startup, image size, and resident memory. M12 will record measurements for those rows and use the specified gate and regression policy. Until then, this page intentionally contains no fabricated numbers.

For reproducible reports, include commit or milestone, machine/OS, compiler and flags, profile (development or runtime), warmup and sample method, measured value, and target/gate. Keep integer replay and raster correctness checks separate from timing results.
