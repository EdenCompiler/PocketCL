# Source and test style

**Status: initial M0 conventions.** These guidelines are intentionally small; the codebase and its Lisp conventions will mature with the compiler and library milestones.

## C kernel

- Write portable C99. Use the project's warning flags and keep interfaces explicit.
- Keep kernel code mechanism-focused. Put language rules, policy, and UI decisions in Lisp unless the roadmap's Rule of Promotion justifies a measured primitive.
- Make ownership, roots, bounds, and error paths clear. Avoid hidden allocation and undefined behavior.

## Lisp sources

- Prefer ordinary ANSI Common Lisp forms and CLHS semantics for supported behavior.
- Keep the compiler bootstrap subset in mind when editing boot/compiler code; the roadmap identifies which files and forms are needed during bootstrap.
- Use names and structure that make generated or macro-expanded behavior inspectable. Add comments for non-obvious invariants rather than restating code.

## Tests and comments

- Write tests around externally visible behavior, determinism, resource boundaries, and error handling.
- Explain important invariants in comments near assertions: what must remain true and what bug the check prevents. For example, ordered traversal protects replay determinism; exhaustion must report failure rather than overrun the arena.
- Keep tests portable across the intended stage-0 and VM runners. SBCL checks are optional.
- Do not claim a feature or milestone is complete until its acceptance checks pass.
