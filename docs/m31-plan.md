# M3.1: terminal diagnostics

Approved on 2026-10-22. Implementation follows successful M3 acceptance and
the user's M3 review; it precedes M4. M3 has passed its local acceptance gates
and is ready for review. This page records the approved next milestone,
not completed diagnostic features.

## Behavior

Use a shared diagnostic record for reader, compiler, VM, loader and host
errors, preserving the original code and condition data. Track source
occurrences through macro expansion and code generation. Report the actual
source file, including failures while building a compiler image.

Default terminal output includes `file:line:column`, a source excerpt,
the message, relevant macro expansions and up to 16 named stack frames.
Unsupported features explain their status and point to the conformance
documentation. Presentation and diagnostic policy remain Lisp; the kernel
exposes source positions and bounded VM frame snapshots.

## Interfaces

- Both development hosts accept `--diagnostics=text|json` (default `text`)
  and `--backtrace-limit=N` (default 16).
- JSON is one diagnostic per line on stderr with `schema_version`, `code`,
  `phase`, `message`, `location`, `macro_expansions`, `frames` and `notes`.
  Program output remains on stdout. JSON mode suppresses plain-text progress.
- Coordinates are one-based UTF-8 byte positions, matching the bootstrap
  reader. Unknown locations remain explicit.
- Detailed debug information lives in optional, versioned Lisp-data sidecars
  associated with an image fingerprint. PFAS v1 and byte-identical compiler
  image comparison remain intact.
- Detailed metadata is development-only. Reporting stays bounded, with an
  allocation-free fallback for heap or stack exhaustion.

## Acceptance

Cover reader errors, invalid lambda lists, macro failures, unsupported
features, runtime type errors, stack exhaustion and malformed FASL files.
Check nested forms, repeated symbols, Unicode and macro-generated code.
Validate JSON escaping, frame limits, missing source files and stale sidecars.
Report each error once without changing handler or unwind behavior.

Rerun cumulative compiler, sanitizer, GC-stress, bootstrap-determinism and
performance gates. Retain the dependency limits, readable code and comments,
16 MiB development arena, and 4 MiB runtime arena.
