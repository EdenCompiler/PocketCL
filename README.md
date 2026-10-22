# PocketCL

PocketCL is a small Common Lisp subset paired with a native UI runtime. **M0–M2 are implemented; the M3 compiler and VM are next.** Cumulative local checks pass strict GCC/Clang builds, sanitizers, full GC stress, 260 portable assertions, 726 native assertions, and one million reader fuzz inputs. The same portable tests pass on SBCL; remote macOS CI remains pending. The design and milestone plan live in [roadmap.md](roadmap.md).

## Project split

The C99 kernel owns low-level mechanisms: values and memory, the bootstrap reader, evaluator/VM, primitive operations, platform adapters, and software rasterization. Lisp owns language and product policy: the compiler, Common Lisp library, conditions and restarts, tooling, tests, reactive UI, layout, styles, widgets, and drawlist generation. Before proposing new C code, first ask whether it is a policy decision that belongs in Lisp; see the Rule of Promotion in [roadmap.md](roadmap.md#32-rule-of-promotion).

## Build and checks

Use `make` for the default build and `make ci` for cumulative local checks. Run `make stage0`, then `build/default/pocketvm0 -e '(+ 20 22)'` or `build/default/pocketvm0 -r` for the bootstrap interpreter. `make diff-sbcl` runs the same portable tests against installed SBCL, which remains an optional oracle rather than a build dependency. The GC-stress target includes the full million-call tail loop and can take several minutes. Remote Linux/macOS workflows are prepared locally; remote execution remains pending. Later targets include `make bootstrap`, `make golden`, and `make bench`. The project adds no runtime dependencies beyond those listed in the roadmap.

## Where to start

For the current kernel, begin with `kernel/include/pocket.h`, then `value.c` for representation, `heap.c` for allocation/GC, `package.c` for names, and `reader0.c`/`debugprint.c` for syntax. Each subsystem has tests under `tests/c/`. Allocators never collect: register live values with `p_roots` before calling the explicit `p_reserve` safe point.

- [CONTRIBUTING.md](CONTRIBUTING.md) explains the language boundary, test expectations, and a useful newcomer path.
- [docs/bootstrap.md](docs/bootstrap.md) follows execution, roots, and allocation for a new contributor.
- [docs/milestones.md](docs/milestones.md) records completed gates and unavailable external checks.
- [docs/spec.md](docs/spec.md) summarizes the language contract and its relationship to the Common Lisp HyperSpec.
- [docs/platform.md](docs/platform.md) records the platform boundary and planned targets.
- [docs/decisions.md](docs/decisions.md) records decisions not fully specified by the roadmap.
- [docs/bytecode.md](docs/bytecode.md) and [docs/drawlist.md](docs/drawlist.md) are reserved for the formats when their implementation milestones define them.
- [docs/perf.md](docs/perf.md) records the measurement baseline and later performance results.

For kernel work, start in `kernel/include/` for public/internal interfaces and `kernel/src/` for implementation. C harness tests live in `tests/c/`; later portable language tests, Lisp unit tests, and visual/replay fixtures are organized under `tests/conformance/`, `tests/lisp/`, `tests/golden/`, and `tests/tapes/`. Empty or placeholder directories mark planned work, not completed features.

At the beginning, most runtime areas are planned rather than implemented. A document may describe a contract or a future target; it does not imply that the corresponding milestone has passed.
