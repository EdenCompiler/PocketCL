# Contributing to PocketCL

PocketCL is being built milestone by milestone. Start with [roadmap.md](roadmap.md), especially the language split, repository layout, testing strategy, and milestone acceptance criteria. See [docs/milestones.md](docs/milestones.md) for observed local results and the current review checkpoint. Remote macOS CI is pending. Later milestones remain planned until their acceptance criteria pass.

## First contribution path

1. Read the project split in the roadmap. Kernel mechanisms are C99; language, library, tooling, tests, and UI policy are Lisp.
2. Find the milestone and acceptance criterion that owns the change. Look for the nearest existing code and tests before adding a new layer.
3. Add a focused test with the behavior. Test observable invariants and failure paths, not just the implementation's internal shape.
4. Run the narrowest relevant check, then the milestone's required `make ci` checks. Optional SBCL differential checks are useful when installed but must not be required to build.
5. Update the relevant docs when a contract or design choice changes. Put decisions the roadmap leaves open in `docs/decisions.md` with date, choice, reason, and rejected alternatives.

## Test invariants

Tests should state what must remain true, ideally in a nearby comment. For example: "iteration follows insertion order so replay hashes do not depend on pointer values" or "allocation exhaustion returns an error and never corrupts the arena." A useful comment explains the invariant and its consequence; it does not narrate each line of the test. Never weaken or delete a failing assertion merely to get a green build.

The roadmap's intended checks grow by milestone: C unit tests first, then portable conformance tests shared between stage 0 and the VM, optional differential checks against SBCL, Lisp unit tests, golden/replay tests, stress and sanitizer runs, and later platform parity. Only describe a check as passing when it has actually run and passed in the current tree.

## C and Lisp boundary

Keep C small and mechanism-focused. If performance appears to require moving Lisp logic into C, follow the roadmap's Rule of Promotion: record a benchmark, try Lisp optimizations first, keep a Lisp reference implementation, and test that both implementations agree. Do not add a runtime dependency to solve a policy problem.

For an execution and memory walkthrough, read [docs/bootstrap.md](docs/bootstrap.md).
It follows a source form from the reader through macro expansion and application,
and shows the explicit root/reserve/allocate pattern. Comments should explain
scope, ownership, and invariants so a new contributor can safely change the code.

## Style and portability

Use portable C99 and the compiler warnings enabled by the project. Keep allocation and error paths explicit. Lisp behavior follows the ANSI Common Lisp HyperSpec (CLHS) wherever PocketCL says a feature is supported; unsupported features should fail clearly rather than silently approximate CL semantics. Read [docs/style.md](docs/style.md) for the project conventions and current maturity notes.
