# Milestone validation

The first delivery ends after M3 acceptance. Local tags mark completed
milestones; a prepared remote workflow is not a claim of a remote CI result.

| Milestone | Local state | Tag |
| --- | --- | --- |
| M0 foundations | Passed | `m0-done` |
| M1 kernel | Passed | `m1-done` |
| M2 bootstrap interpreter | Passed | `m2-done` |
| M3 compiler and self-hosting | Passed locally; ready for user review | `m3-done` |
| M3.1 terminal diagnostics | Approved; follows M3 acceptance and user review | Pending |

The approved [M3.1 diagnostics plan](m31-plan.md) adds terminal text/JSON
errors, source and macro traces, named frames, and development debug sidecars.
It preserves the original M3 delivery/review checkpoint before that work starts.

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

## M3 checks — 2026-10-23

The delivery tree has passed strict C99 GCC 14.2.0 and Clang 19.1.7 builds,
ASAN/UBSAN, and cumulative GC-stress checks, including:

- 1,062 native assertions across nine suites.
- 277 portable assertions on both the interpreter and image-hosted VM, with
  the same 277 results on SBCL 2.5.2.
- 38 compiler assertions, 12 compiler edge assertions, 12 FASL round-trip
  assertions, and 11 VM acceptance assertions.
- 623 independent Lisp writer/table assertions.
- Byte-identical stage-1/stage-2 images in the GCC, Clang and sanitizer builds.
- One million reader inputs and one million malformed-FASL inputs under
  ASAN/UBSAN.

The full `make stress` run completed, including the interpreter's million-call
case. The native host's CLI was then corrected to preserve the compile-time
GC-stress default. Its complete `test-vm bootstrap` targets were rerun with
`-DPOCKET_GC_STRESS=1`, passing the portable, compiler, FASL, overflow, retained
memory and byte-identical image checks. No stress case was shortened or
excluded. Final `make gcc clang asan` also passed after this host-only change.
The callback root regression is covered by native tests. The native executable
contains no tree-evaluator entry points. The 4 MiB arena configuration passes
all 11 VM acceptance assertions.

Stage-1 and stage-2 compiler images are each 184,060 bytes and have SHA-256
`e339c6eba875c6a811c9ce4993ca4f3429e34c9e05bcff84e776fae37e8c73e9`.
The default, GCC, Clang, sanitizer and GC-stress images all match.

The reference-machine Fibonacci benchmark passed at **16.50×**: median
14.85 seconds in stage 0 and 0.90 seconds in the VM. Retained VM heap is
identical after 1,000 and 1,000,000 tail calls. Reproduction commands,
samples and memory/size limits are in [perf.md](perf.md).

SBCL 2.5.2 passes the same 277 portable assertions with no result differences.
Its source warnings have the meanings listed under M2 above. It also prints
load-context lines for deliberately signaled conditions that tests handle;
these do not indicate unhandled errors or failed assertions.

M3 is the completed first-phase delivery and user review checkpoint.
M3.1 implementation starts only after that review. M4 and later milestones
remain planned; remote/platform checks listed above remain pending.
