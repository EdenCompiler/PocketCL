# Design decisions

Decisions made where the roadmap leaves an implementation choice open. Add new entries with a date, decision, reason, and rejected alternatives. The roadmap remains the source of the broader design and milestone requirements.

## 2026-10-20 — minimal conditions before M3

**Decision:** implement the smallest useful PocketCL condition support before the full condition-system milestone, so early kernel/evaluator errors can be reported clearly and later wired into standard conditions.

**Reason:** bootstrap development and tests need useful failures before the compiler/VM is ready. This early mechanism does not claim that the full Common Lisp condition system is complete; that remains later library work.

**Alternatives rejected:** postpone all structured errors until M4, or implement a separate ad hoc error language that would need to be replaced.

## 2026-10-20 — compiler bootstrap representation

**Decision:** bootstrap compiler intermediate data uses tagged vectors.

**Reason:** tagged vectors are simple to construct with the early runtime and can represent compiler nodes without adding a new kernel object type.

**Alternatives rejected:** add compiler-specific C object types or use untagged positional lists whose node kinds are less explicit.

## 2026-10-20 — fixnum range and unsigned encoding

**Decision:** use 31-bit signed fixnums on 32-bit value words and 62-bit signed fixnums on 64-bit value words; encode/decode through unsigned arithmetic.

**Reason:** reserve tag bits while keeping the payload's signed range explicit, and avoid relying on implementation-defined signed shifts or overflow.

**Alternatives rejected:** assume signed shifts are arithmetic on every C99 compiler, or perform overflowing signed shifts during encode/decode.

## 2026-10-20 — allocation preflight

**Decision:** preflight allocation capacity and return an error on exhaustion; primitives do not trigger garbage collection.

**Reason:** explicit failure keeps primitive behavior simple and makes allocation boundaries testable before roots and collection are fully established. Collection policy belongs in controlled runtime points.

**Alternatives rejected:** collect implicitly inside arbitrary primitives or allow arena exhaustion to corrupt memory.

## 2026-10-20 — initial arena defaults

**Decision:** use a 16 MiB development arena and a 4 MiB runtime arena as initial defaults.

**Reason:** development benefits from room for diagnostics and early tools; the runtime default starts within the project's constrained-memory goal. These are defaults to measure and revisit, not proven final budgets.

**Alternatives rejected:** one shared size for both profiles or choosing a smaller runtime arena before workload measurements exist.

## 2026-10-20 — floating-point portability

**Decision:** permit platform variation in floating-point results; require exact integer replay and determinism for integer-based layout and raster behavior.

**Reason:** C99 and target math libraries can differ in floating-point details. Avoid promising cross-platform bitwise equality for computations the design does not make integer-only.

**Alternatives rejected:** claim bit-identical floating-point results across all targets without a controlled software-float implementation.

## 2026-10-20 — performance reference machine

**Decision:** use the local AMD Ryzen 5 5500U as the initial desktop baseline machine.

**Reason:** this is the available machine for development measurements. Record compiler, flags, and workload with results before treating measurements as comparable.

**Alternatives rejected:** treat roadmap proxy budgets as already measured or claim a generic desktop baseline.

## 2026-10-20 — CI availability

**Decision:** GitHub Actions is the intended remote CI service, with Linux and macOS jobs, but it is not configured yet.

**Reason:** preserve the planned platform coverage while accurately reflecting the current repository state.

**Alternatives rejected:** describe remote CI as active before workflows and repository integration exist.

## 2026-10-20 — M14 PSP target and ASCII font

**Decision:** target PSP for M14 and use PPSSPP as the intended emulator; keep M13 deferred. The initial built-in font is 8×8 ASCII with a fallback glyph.

**Reason:** choose one constrained target early and keep the first font independent of runtime font tooling.

**Alternatives rejected:** leave the device undecided or make M14 depend on M13's optional font pipeline.
