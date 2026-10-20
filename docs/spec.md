# PocketCL language contract

PocketCL is intended to be a small, explicit subset of ANSI Common Lisp, not a new dialect. For any feature marked supported, the Common Lisp HyperSpec (CLHS) defines its semantics, argument evaluation, and options. This document summarizes project policy; it does not replace the CLHS. The complete symbol-by-symbol implementation status belongs in `docs/conformance.md` and is maintained separately.

## Current status

**M0 foundations are complete; M1–M12 and M14 are planned.** This file is an initial contract, not evidence that a reader, evaluator, compiler, condition system, or library feature is implemented. Follow [roadmap.md](../roadmap.md) for milestone scope and acceptance criteria.

## Language and implementation policy

- C99 supplies the kernel mechanisms defined in the roadmap: representation and memory, bootstrap execution, primitive operations, VM, and platform-facing code.
- Lisp supplies the compiler, standard-library policy, tooling, tests, UI behavior, layout, style resolution, and drawlist construction.
- The intended conformance rule is: supported behavior follows the CLHS; unsupported behavior reports a clear PocketCL unsupported-feature condition or a build-time error naming the symbol. It must not silently act like a different feature.
- `COMMON-LISP` exports only symbols that PocketCL implements. The eventual conformance table must identify each standard symbol as supported, partial with notes, or excluded.
- SBCL may be used as an optional differential oracle. It is not required to build or run PocketCL.

## Scope

The roadmap defines the planned subset and explicit v1 exclusions. Among the exclusions are CLOS/MOP, bignums, ratios and complex numbers, threads, pathnames, a JIT, and browser DOM/CSS compatibility. Consult the roadmap and the conformance table rather than inferring support from a familiar symbol name.

The UI API and exact language differences will be documented here as their milestones establish them. Until then, this page records only the governing contract and does not invent semantics.
