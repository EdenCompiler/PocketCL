# Milestone validation

The first delivery ends after M3 acceptance. Local tags mark completed
milestones; a prepared remote workflow is not a claim of a remote CI result.

| Milestone | Local state | Tag |
| --- | --- | --- |
| M0 foundations | Passed | `m0-done` |
| M1 kernel | Passed | `m1-done` |
| M2 bootstrap interpreter | Passed | `m2-done` |
| M3 compiler and self-hosting | Design reviewed; implementation has not begun | Pending |

## M2 checks — 2026-10-21

The frozen integration tree has passed:

- Strict C99 GCC 14.2.0 and Clang 19.1.7 builds (`make gcc clang`).
- 726 native assertions across six C suites.
- 260 portable language assertions: binding and namespace scopes, closures
  and mutation, rich lambda lists and destructuring, macros and their lexical
  environments, special declarations and bindings, control/unwind ordering,
  multiple values, packages, and a million-iteration tail loop.
- The same 260 assertions on installed SBCL 2.5.2 (`make diff-sbcl`).
- ASAN and UBSAN across native and portable suites (`make asan`).
- One million deterministic reader fuzz inputs under the sanitizers.
- Offline inventory/table agreement for all 978 standard names and runtime
  role/profile agreement for the 196 current standard exports.

The full `make stress` run passed with exit status zero. It includes the same
million-call case and collects at every allocation safe point. It was not
shortened or excluded because of its runtime. The GCC, Clang, ASAN/UBSAN, and
stress targets together comprise the cumulative `make ci` gate.

SBCL reports no assertion differences. Its diagnostics include unused variables,
the intentionally combined optional/keyword lambda list, integer catch-tag
portability, and an intentionally accepted declaration naming an unused
variable. These are source diagnostics rather than differing test results.
The harness uses reader conditionals solely for host reporting/error plumbing;
the package assertions have the same semantics and count on both hosts.

For retained-memory measurements and preliminary stage-0 timings, see
[perf.md](perf.md). For build/run commands and a newcomer walkthrough, see
[bootstrap.md](bootstrap.md).

## External checks

Linux checks above run on the local reference machine. Linux/macOS GitHub
Actions workflows are prepared locally; remote publishing and execution remain
pending. No macOS, 32-bit, browser, PSP, or emulator result is claimed here.
The latter platform ports belong to later milestones.
