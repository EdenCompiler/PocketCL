# PocketCL

PocketCL is a small Common Lisp subset paired with a native UI runtime. **M0 foundations are complete; M1 kernel core is next.** Local `make ci` checks pass; remote macOS CI is still pending. The design and milestone plan live in [roadmap.md](roadmap.md); this README describes how to navigate the repository as the implementation grows.

## Project split

The C99 kernel owns low-level mechanisms: values and memory, the bootstrap reader, evaluator/VM, primitive operations, platform adapters, and software rasterization. Lisp owns language and product policy: the compiler, Common Lisp library, conditions and restarts, tooling, tests, reactive UI, layout, styles, widgets, and drawlist generation. Before proposing new C code, first ask whether it is a policy decision that belongs in Lisp; see the Rule of Promotion in [roadmap.md](roadmap.md#32-rule-of-promotion).

## Build and checks

Use `make` for the default build and `make ci` for the current M0 checks. The local CI workflow builds/runs the C harness with GCC, Clang, ASAN/UBSAN, and the GC-stress configuration, plus the conformance-table check. Remote Linux/macOS CI is planned; macOS has not yet been exercised remotely. The roadmap's later targets include `make stage0`, `make bootstrap`, `make golden`, optional `make diff-sbcl`, and `make bench`; consult the current Makefile because targets arrive with their milestones. SBCL is an optional test oracle, never a build dependency. The project adds no runtime dependencies beyond those listed in the roadmap.

## Where to start

- [CONTRIBUTING.md](CONTRIBUTING.md) explains the language boundary, test expectations, and a useful newcomer path.
- [docs/spec.md](docs/spec.md) summarizes the language contract and its relationship to the Common Lisp HyperSpec.
- [docs/platform.md](docs/platform.md) records the platform boundary and planned targets.
- [docs/decisions.md](docs/decisions.md) records decisions not fully specified by the roadmap.
- [docs/bytecode.md](docs/bytecode.md) and [docs/drawlist.md](docs/drawlist.md) are reserved for the formats when their implementation milestones define them.
- [docs/perf.md](docs/perf.md) records the measurement baseline and later performance results.

For kernel work, start in `kernel/include/` for public/internal interfaces and `kernel/src/` for implementation. C harness tests live in `tests/c/`; later portable language tests, Lisp unit tests, and visual/replay fixtures are organized under `tests/conformance/`, `tests/lisp/`, `tests/golden/`, and `tests/tapes/`. Empty or placeholder directories mark planned work, not completed features.

At the beginning, most runtime areas are planned rather than implemented. A document may describe a contract or a future target; it does not imply that the corresponding milestone has passed.
