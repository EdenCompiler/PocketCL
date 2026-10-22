# Bootstrap implementation guide

M0–M2 have passed their local acceptance gates. The commands below exercise
the stage-0 interpreter and bootstrap library.
The later compiler/VM bootstrap is specified in [the roadmap](../roadmap.md#62-bootstrap-sequence).

## Build and run stage 0

From the repository root:

```sh
make stage0
build/default/pocketvm0 -e '(+ 20 22)'
build/default/pocketvm0 -r
build/default/pocketvm0 tests/conformance/portable-harness.lisp tests/conformance/core-test.lisp
make diff-sbcl
make ci
```

The executable embeds the source directory at build time, so it can find the
bootstrap files when run from another working directory. `--boot-dir PATH`
selects a different bootstrap directory. `--no-boot` leaves only the native
operators and primitives for kernel experiments; `--stress` collects at every
allocation safe point. These switches apply before any source operands.

Source operands and `-e FORM` operands run in order in the same image. The
stage-0 REPL accepts one line at a time, uses the diagnostic printer, and
reports reader positions. Full streams, reader customization, and a richer
REPL arrive at M6.

`--exports` lists the actual external Common Lisp symbols and their current
binding kinds. `--stats` collects at an explicit safe point and reports arena,
heap, retained bytes, and collection count. It can be repeated between
workloads to distinguish retained objects from allocation churn.

## Follow one function call

Start with `kernel/include/pocket.h` for Values and object layouts, then
`kernel/include/eval.h` for callable objects and evaluator state. The relevant
paths are:

1. `main0.c` initializes the arena, evaluator, and primitives.
2. `boot.c` loads each bootstrap file. Its source loop reads and evaluates one
   form at a time, so package changes affect the next form's reader.
3. `reader0.c` returns a Value tree. Reader errors carry line and column.
4. `eval0.c` resolves native special operators, lexical bindings, and macros.
   An ordinary call evaluates arguments from left to right, then applies the
   function from the separate lexical/global function namespace.
5. A `PClosure` holds parameters, body, and its captured environment. A
   `PPrimitive` holds a native entry point, arity, and allocation estimator.
6. `prim.c` performs the small native operation. File and console operations
   cross the host interface in `host.h`/`host.c`.

Lexical environments are tagged records with a parent and separate binding
lists for values, functions, macros, symbol macros, blocks, and tags. A
binding is a mutable cons cell; closures that capture the same binding see
later assignments. Global symbols also have distinct value and function
cells. Special variables use shallow dynamic binding, saving the old value
on the dynamic stack and restoring it during normal or non-local exit.

Stage 0 uses a trampoline for tail calls and bounded C recursion for other
evaluation. Non-local exits use explicit control/dynamic stacks and
`setjmp`/`longjmp`. The VM milestone replaces tree walking with bytecode and
explicit VM frames; it must not hide calls back to the tree evaluator.

## Keep Values alive

This is a non-moving collector, but an unrooted Value can still become a
freed object at the next safe point. Constructors such as `p_cons` and
`p_vector` allocate without collecting. `p_reserve` is the collecting boundary:

```c
Value live[2] = {input, 0u};
size_t saved = p_root_save(p);
if (!p_roots(p, live, 2u) || !p_reserve(p, sizeof(PCons))) {
    p_root_restore(p, saved);
    return 0u;
}
live[1] = p_cons(p, live[0], p->nil);
Value result = live[1];
p_root_restore(p, saved);
return result;
```

Initialize every registered slot. Register the input before a safe point,
including one inside a helper. Restore the root count on every return path.
An internal zero Value means failure; Lisp NIL is the permanent NIL symbol.
Never call Lisp or collect from a primitive entry: its reserve estimator runs
first while the evaluator still roots every argument.

## Why the bootstrap is Lisp

`lisp/boot/00-core.lisp` starts with required/rest parameters and native forms.
It builds list helpers, definition macros, control macros, backquote, and
package helpers. `10-lambda-list.lisp` lowers optional, keyword, auxiliary,
and destructuring parameters into the same small core. `20-cl-macros.lisp`
adds further derived forms. This shared expansion must work on both the
interpreter and the later compiled runtime.

Keep language policy here: equality over compound objects, macro expansion,
lambda-list validation, and condition dispatch belong in Lisp. A new C
primitive should expose a mechanism the existing language cannot implement,
with an explicit allocation bound and focused native tests.

Portable assertions live in `tests/conformance/`; the same source runs on
PocketCL and optional SBCL. Reader conditionals select only host reporting
plumbing. Native tests cover mechanisms and failures that need direct arena
inspection. The manifest/table checks are offline and require no host Lisp.
