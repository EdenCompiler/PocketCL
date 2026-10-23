# PocketCL

PocketCL is a small Common Lisp subset paired with a planned native UI runtime. **M0–M3 have passed their local acceptance gates; M3 is ready for review.** The Lisp compiler rebuilds byte-identical images on the native VM. Completed milestone results are recorded in [docs/milestones.md](docs/milestones.md). The design and milestone plan live in [roadmap.md](roadmap.md).

## Project split

The C99 kernel owns low-level mechanisms: values and memory, the bootstrap reader, evaluator/VM, primitive operations, platform adapters, and software rasterization. Lisp owns language and product policy: the compiler, Common Lisp library, conditions and restarts, tooling, tests, reactive UI, layout, styles, widgets, and drawlist generation. Before proposing new C code, first ask whether it is a policy decision that belongs in Lisp; see the Rule of Promotion in [roadmap.md](roadmap.md#32-rule-of-promotion).

## Build and checks

Use `make` for the default build and `make ci` for cumulative local checks. Run `build/default/pocketvm -e '(+ 20 22)'` or `build/default/pocketvm -r` for the image-hosted VM; `build/default/pocketvm0` provides the bootstrap interpreter. `make bootstrap` builds and compares the two compiler images; `make benchmark` runs the Linux reference timing gate. `make diff-sbcl` runs the portable tests against installed SBCL, which remains an optional oracle rather than a build dependency. The GC-stress target includes the full million-call cases and can take over an hour on the reference laptop. Remote Linux/macOS workflows are prepared locally; remote execution remains pending. The project adds no runtime dependencies beyond those listed in the roadmap.

## Where to start

For the current kernel, begin with `kernel/include/pocket.h`, then `value.c` for representation, `heap.c` for allocation/GC, `package.c` for names, and `reader0.c`/`debugprint.c` for syntax. Each subsystem has tests under `tests/c/`. Allocators never collect: register live values with `p_roots` before calling the explicit `p_reserve` safe point.

- [CONTRIBUTING.md](CONTRIBUTING.md) explains the language boundary, test expectations, and a useful newcomer path.
- [docs/bootstrap.md](docs/bootstrap.md) follows execution, roots, and allocation for a new contributor.
- [docs/milestones.md](docs/milestones.md) records completed gates and unavailable external checks.
- [docs/spec.md](docs/spec.md) summarizes the language contract and its relationship to the Common Lisp HyperSpec.
- [docs/platform.md](docs/platform.md) records the platform boundary and planned targets.
- [docs/decisions.md](docs/decisions.md) records decisions not fully specified by the roadmap.
- [docs/bytecode.md](docs/bytecode.md) defines VM instructions and PFAS v1. [docs/drawlist.md](docs/drawlist.md) remains reserved for the later UI milestone.
- [docs/m31-plan.md](docs/m31-plan.md) records the approved diagnostics work after M3 passes and is reviewed.
- [docs/perf.md](docs/perf.md) records the measurement baseline and later performance results.

For kernel work, start in `kernel/include/` for public/internal interfaces and `kernel/src/` for implementation. C harness tests live in `tests/c/`; portable language tests and Lisp unit tests are in `tests/conformance/` and `tests/lisp/`. Later visual/replay fixtures belong in `tests/golden/` and `tests/tapes/`. Empty or placeholder directories mark planned work, not completed features.

At the beginning, most runtime areas are planned rather than implemented. A document may describe a contract or a future target; it does not imply that the corresponding milestone has passed.
