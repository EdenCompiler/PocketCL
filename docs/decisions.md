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

**Decision:** prepare GitHub Actions workflows locally with Linux and macOS jobs. Remote publishing and execution remain a separate step.

**Reason:** preserve the planned platform coverage while accurately reflecting the current repository state.

**Alternatives rejected:** describe remote CI as active before repository integration and observed runs exist. The macOS GCC job explicitly selects Homebrew's GNU compiler because Apple's `gcc` command invokes Clang.

## 2026-10-20 — M14 PSP target and ASCII font

**Decision:** target PSP for M14 and use PPSSPP as the intended emulator; keep M13 deferred. The initial built-in font is 8×8 ASCII with a fallback glyph.

**Reason:** choose one constrained target early and keep the first font independent of runtime font tooling.

**Alternatives rejected:** leave the device undecided or make M14 depend on M13's optional font pipeline.

## 2026-10-20 — kernel headers, roots, and diagnostic limits

**Decision:** object headers store type in bits 0–7, the mark bit at 8, and size in eight-byte units from bit 10. One startup allocation contains both the heap and an iterative collector work stack; its total is the configured arena size. Small free blocks have eight-byte size classes; large blocks use first fit and sweep coalesces adjacent free blocks.

**Reason:** byte-size encoding above bit 16 cannot represent a multi-megabyte free block on a 32-bit PSP. Unit encoding preserves the one-word header without this limit. Explicit safe points and root ranges allow constructors to remain non-collecting. Internal Value pointers must designate object starts; the collector validates bounds/header rather than performing a quadratic search per edge.

**Alternatives rejected:** separately allocated GC metadata, post-initialization host reallocations, implicit constructor collection, recursive marking, and a full heap scan for every pointer.

The C `p_equal` helper is a bounded diagnostic comparator for reader tests, including vectors and finite cyclic graphs. It uses fixed local work arrays and returns false when its 1024-pair budget is exceeded. It is not CL:EQUAL; language equality belongs in Lisp. The diagnostic printer similarly emits stable cycle/depth markers. The bootstrap reader has explicit 256-form/4095-byte limits and reports excesses rather than exhausting C stacks.

## 2026-10-23 — compiler metadata and reproducible images

**Decision:** keep analysis records and identity tables in Lisp. Metadata is
associated with both a source form and its lexical context. Assign FASL graph
IDs through source-ordered buffers, using hashes only for lookup.

**Reason:** a macro can reuse one cons tree in different scopes; an object
address alone cannot identify the binding at each occurrence. Address-based
hash bucket order must not affect image bytes. Tagged vectors remain usable
before the M4 structure and generalized-place libraries exist.

**Alternatives rejected:** compiler-specific C records, emission by hash-table
iteration order, and storing native arena addresses in an image.

PFAS v1 uses fixed-width little-endian operands and signed relative jumps.
The C loader checks bounds, references, instructions and stack joins before
materializing package symbols. Lisp controls serialization and top-level
compile/load effects. Optional development debug sidecars are M3.1 work and
must preserve the PFAS v1 fixed-point comparison.

## 2026-10-23 — bounded VM errors and bootstrap engines

**Decision:** use explicit arena-backed VM frames, dynamic entries and an
ordinary Lisp error hook. Reserve 64 emergency frames, 1,024 operand values
and 64 KiB of heap for signaling and cleanup. Native primitive entries still
never invoke Lisp or collect.

**Reason:** stack exhaustion must reach handlers before unwinding, and cleanup
closures must execute without recursive C entry into the VM. The final native
host excludes the tree evaluator. The source-development host compiles the
condition dispatcher when entering `--vm` mode, keeping its hooks and handlers
on the same control engine.

**Alternatives rejected:** unbounded C recursion, silently growing the arena,
or attempting to allocate reporting capacity after ordinary resources run out.

This early condition subset preserves handler order, dynamic restoration and
multiple values. General condition classes, restarts and debugger behavior
remain M4. M3.1 supplies the approved diagnostic presentation layer after the
M3 review checkpoint.
