# Pocket Common Lisp: Roadmap and Design Document

**Status:** design v2.0, 2026-10-19 (supersedes the Scheme-flavored v1.0)
**Audience:** an AI coding agent that will build this project end to end. Humans may read it, but it is written to be executed.
**Inspiration:** [PocketJS](https://pocketjs.pocket.nexus/): a compact native runtime plus familiar components, no browser engine, tiny memory, one app on many screens.

---

## 0. Instructions to the builder (read first)

Keywords **MUST**, **SHOULD**, **MAY** are used in the RFC 2119 sense.

1. **Work milestone by milestone, in order** (section 13). Do not start milestone *N+1* until every acceptance criterion of *N* passes via `make ci`.
2. **Write the tests with (or before) the code.** A milestone with failing or missing tests is not done.
3. **Respect the language split** (section 3). C is for the kernel and thin platform backends. All logic is Lisp. Before writing C for anything not listed in section 3.1, apply the *Rule of Promotion* (section 3.2).
4. **PocketCL is a subset of ANSI Common Lisp, not a new dialect.** For every supported feature, the Common Lisp HyperSpec (CLHS) is the authority. If this document and the CLHS disagree on a supported feature, the CLHS wins unless the difference is listed in section 4.15 (deviations). Do not invent semantics.
5. **Deviations fail loudly.** Anything PocketCL does not implement MUST signal a clear `pocket:unsupported-feature` error (or be a build-time error naming the symbol). It MUST NOT silently behave differently from ANSI.
6. **No new dependencies** beyond those in section 3.4.
7. **Record every decision not covered here** in `docs/decisions.md` (date, decision, reason, alternatives rejected). Prefer the simplest option that satisfies the spec.
8. **Keep docs current:** `docs/conformance.md` (per-symbol status of every `COMMON-LISP` symbol), `docs/bytecode.md`, `docs/drawlist.md`, `docs/platform.md`, `docs/style.md`, `docs/decisions.md` MUST match the code at each milestone tag.
9. **If this document contradicts itself or is silent,** pick the simplest consistent option, record it, and continue. Do not stall waiting for answers.
10. **Never weaken a test to make it pass.** Never disable GC-stress or sanitizer runs to hide a bug.
11. **Commits:** one logical change each. Tag finished milestones `m0-done`, `m1-done`, and so on.
12. **Determinism is a feature** (section 2). Anything observable from Lisp that depends on wall-clock time, memory addresses, or hash-seed randomness is a bug.

---

## 1. Vision and scope

Pocket Common Lisp (**PocketCL**) is a small, honest subset of Common Lisp plus a native UI runtime inspired by PocketJS. You write apps as Lisp components using fine-grained reactivity (signals); the runtime lays them out with flexbox and draws them without a browser engine, a VDOM, or a large host language. It should fit on constrained hardware (a handheld-class CPU and single-digit megabytes of RAM) and also run on desktop and in the browser.

**The split that defines the project:** a small **C kernel bootstraps the system**; **everything else is written in PocketCL**, including the compiler, the Common Lisp standard library, the full reader and printer, the condition system, the reactive runtime, the layout engine, the style system, widgets, tooling, and the test framework.

### Goals

- **G1.** A self-hosting toolchain: the Lisp compiler compiles itself to a fixed point.
- **G2.** A portable C99 kernel with a tiny platform ABI.
- **G3.** A real Common Lisp subset: existing CL knowledge, CL code, and CL test material transfer, and behavior can be checked against a reference implementation (SBCL) when available.
- **G4.** A UI stack (reactivity, flexbox layout, text, input and focus, drawing) written in Lisp.
- **G5.** Deterministic, frame-clocked execution with record and replay, testable headless.
- **G6.** Build-time work over run-time work: styles, `format` strings, and (later) animations are baked by macros and compiler macros.
- **G7.** Small footprint, measured and reported; tree-shaking so unused library code costs nothing; budgets enforced in CI.

### Non-goals (v1)

CLOS and the MOP (`defclass`, `defgeneric`, `defmethod`), bignums, ratios and complex numbers, the pretty printer, pathnames and logical pathnames, threads, `load-time-value`, `progv`, a JIT, a browser-compatible DOM or CSS, accessibility APIs, right-to-left or complex text shaping, a package manager, GPU backends. See M15+ in section 13 for the backlog.

---

## 2. Design principles

1. **Subset, not dialect.** Supported features behave per the CLHS. Unsupported features fail loudly. The `COMMON-LISP` package exports only what is implemented.
2. **Small kernel.** If it can be written in Lisp, it is written in Lisp.
3. **Deterministic by construction.** State advances one `frame` call at a time. Time is measured in frames, never wall-clock. Hash-table iteration and package-symbol iteration are insertion-ordered. Argument evaluation is left to right (as in CL). Nothing observable depends on pointer values.
4. **Idle is free.** If nothing is dirty, a frame does no layout, no drawing, and no presenting.
5. **Headless first.** Every feature is testable without a window, via a headless backend that produces a framebuffer hash.
6. **Bake early.** Class strings, `format` control strings, and later keyframes are resolved at compile time into constants or direct code.
7. **One renderer contract.** Lisp emits a flat *drawlist* (a byte vector). C consumes it. Nothing else crosses the render boundary.
8. **Portable C99.** No compiler extensions required, a short libc list (section 3.4), one fixed-size memory arena, no `malloc` after init in the kernel.
9. **Pay for what you use.** The full CL library is written in Lisp, so the linker's tree-shaker drops everything an app does not reference.

---

## 3. Architecture

### 3.1 Language split

| Component | Language |
|---|---|
| Value representation, heap arena, mark-sweep GC | **C** |
| Symbols (name, package, value cell, function cell, plist, flags) and the package table (`intern`, `find-symbol`, `export`, `import`, `shadow`, `use-package` primitives) | **C** |
| **Bootstrap reader** (fixed standard readtable, no user reader macros) and **debug printer** (used only for boot, crash reports, and early errors) | **C** |
| Stage-0 bootstrap evaluator (tree-walker, used only to build the compiler) | **C** |
| Bytecode VM (including dynamic-state stack, special bindings, multiple values), fasl loader | **C** |
| Primitives: conses, fixnum and flonum arithmetic, characters, simple vectors, simple strings, byte vectors, insertion-ordered hash tables, records (structures), symbol cells, basic console I/O, GC control | **C** |
| Platform backends: window and input polling, framebuffer present, file access, pacing; later net and audio | **C** |
| Software rasterizer (drawlist to pixels; integer-only; policy-free) | **C** (shared by all backends) |
| **Macro expander, compiler** (analysis, optimization, codegen), fasl writer, linker, tree-shaker, `defsystem`-lite, CLI, REPL logic | **Lisp** |
| **The Common Lisp library:** lambda-list processing, `defmacro`/`defun`/`setf`/`defstruct`/`loop`/`do`/`case`/`typecase`/`destructuring-bind`, types, **condition system and restarts**, sequences, lists, strings, characters, numbers (above fixnum/flonum primitives), hash-table API, arrays, **full Lisp reader (readtables)**, **full Lisp printer**, streams, `format`, packages API (`defpackage`, `in-package`, ...), `random` | **Lisp** |
| Test framework, benchmarks, asset and font tools, devtools, examples | **Lisp** |
| Reactive runtime, UI tree, style resolver and class baking, flexbox layout, text layout, input and focus, widgets, drawlist emitter, frame loop | **Lisp** |

The software rasterizer is in C on purpose: it is platform glue (pixel pushing with no policy), and a Lisp VM cannot fill pixels fast enough on small CPUs. All *decisions* about what to draw are Lisp.

### 3.2 Rule of Promotion

Logic may move from Lisp to C **only if all of these hold**:

1. A benchmark in `bench/` shows the Lisp version breaks a budget in section 12.
2. Optimizing the Lisp (algorithm, allocation, data layout, declarations, compiler macros) was tried first and recorded in `docs/decisions.md`.
3. The C function is a *pure primitive* (no policy, no callbacks into Lisp, no signaling beyond requesting an error from the VM).
4. The original Lisp implementation stays in the tree as `reference-<name>` and a test asserts the C and Lisp versions agree on a randomized corpus.

### 3.3 Pipeline

```
 build time (desktop, `pocket build`)                run time (any target)
 ---------------------------------                   ---------------------
 *.lisp ──► reader (Lisp)                            app.fasl (bytecode image)
        ──► expander (Lisp): macros, setf, loop,        │
            style baking, format baking                 ▼
        ──► analyzer/optimizer (Lisp)                ┌──────────────┐
        ──► codegen + linker + tree-shaker (Lisp)    │ bytecode VM  │ (C)
        ──► app.fasl ────────────────────────────────►              │
                                                     └──────┬───────┘
                                                            │ runs Lisp:
 component code ► reactive runtime ► UI tree ► layout ► drawlist
                                                            │ (Lisp fills a byte vector)
                                                     ┌──────▼───────┐
                                                     │ host-present │ (C)
                                                     │ rasterizer   │ (C)
                                                     │ platform     │ (C)
                                                     └──────┬───────┘
                                                          pixels
```

### 3.4 Allowed dependencies

- **Kernel:** C99 compiler; libc `memcpy memmove memset memcmp strlen strcmp`, plus `snprintf` and `strtod` for float formatting and parsing (isolated in one file), and `libm` (`sqrt exp log sin cos tan atan2 pow floor ceil`) used only by numeric primitives. No other libraries.
- **Desktop backend:** SDL2 (window, input, texture upload). Nothing else.
- **WASM backend:** Emscripten.
- **Host-only asset tool (M13, optional):** vendored `stb_truetype.h` (public domain), exposed only in desktop tool builds, never in runtime-only images.
- **Build:** POSIX `make` and a shell. No CMake, no Python in the build path.
- **Optional test oracle:** SBCL, if present on the machine, for differential testing (section 11). Never required.

### 3.5 Targets

| Tier | Target | Backend | Milestone |
|---|---|---|---|
| 1 | Linux and macOS, headless | `platform/headless` | M7 |
| 1 | Linux and macOS desktop window | `platform/sdl2` | M10 |
| 2 | Browser via WebAssembly (playground) | `platform/wasm` | M11 |
| 3 | One constrained device (choose one: PSP via PSPSDK, 3DS via libctru, or ESP32-P4 via ESP-IDF; emulator acceptable) | `platform/<device>` | M14 |

Tier 3 uses the same software rasterizer first. GPU backends are an optional later optimization.

---

## 4. Language specification (PocketCL)

The full text goes in `docs/spec.md` (a short document that *refers to the CLHS* and lists only what differs) and `docs/conformance.md` (one line per `COMMON-LISP` symbol: supported, partial with notes, or excluded). This section is normative for the essentials.

### 4.1 The conformance contract

1. **Every symbol exported from `COMMON-LISP` is implemented** with ANSI semantics for the lambda lists and options documented in `docs/conformance.md`. Options that are not implemented signal `pocket:unsupported-feature`.
2. **Every standard symbol that is not implemented is not exported** from `COMMON-LISP`. Referencing one (for example `defclass`) is a **build-time error** that names the symbol and points to `docs/conformance.md`. `docs/conformance.md` is generated and checked against the list of all standard symbols.
3. **Extensions live in package `POCKET`** and never shadow or extend `COMMON-LISP` symbols.
4. `*features*` includes `:pocket`, `:pocket-cl`, `:ieee-floating-point`. It does **not** include `:ansi-cl` or `:common-lisp`, because PocketCL is a subset.
5. **Portable-subset rule:** any program that uses only supported features behaves the same under PocketCL and a conforming implementation (modulo section 4.15), so it can be checked against SBCL.

### 4.2 Evaluation model

**Lisp-2** (separate function and value namespaces: `#'f`, `funcall`, `flet`, `labels`), lexically scoped by default, eager, single-threaded. **Arguments are evaluated left to right.** `NIL` is both false and the empty list; every non-`NIL` value is true. `T` is the canonical truth value.

**Proper tail calls are guaranteed in compiled code** (stronger than ANSI): a call in tail position is a jump, **except** when a dynamic-extent construct is active in the same function (special-variable binding, `catch`, `unwind-protect`, `handler-bind`/`handler-case` establishing handlers, `restart-case`). This is required: the frame loop and many reactive patterns depend on it.

### 4.3 Reader syntax (standard readtable, subset)

| Syntax | Meaning |
|---|---|
| `; ...` and `#\| ... \|#` (nestable) | comments |
| `( ... )` `.` | lists and dotted pairs |
| `'x` `` `x `` `,x` `,@x` | quote, backquote, comma, comma-at (standard CL backquote semantics) |
| `#'f` | `(function f)` |
| `#( ... )` | simple vector literal |
| `#\a` `#\Space` `#\Newline` `#\Tab` `#\Return` `#\x41` (hex via `#\U+...` optional) | characters |
| `"..."` | strings; escape only `\"` and `\\` (standard CL) |
| `42` `-7` `#x1F` `#b1010` `#o17` `3.14` `1e3` `1.5d0` | integers (fixnum range) and floats |
| `foo` `:key` `pkg:sym` `pkg::sym` `#:gensym` `\|Mixed Case\|` | symbols (see 4.8 and the case rule below) |
| `#+feature form` / `#-feature form` | feature-expression conditionals (`and or not`, keywords) |
| `#.form` | read-time evaluation, **only when `*read-eval*` is true** (default `NIL`) |

**Symbol case:** the standard `:upcase` readtable case. `foo` reads as `FOO`; `(symbol-name 'foo)` is `"FOO"`. The printer follows the standard rules. This keeps PocketCL source portable and comparable with SBCL.

**Two readers.** The **bootstrap reader** (C, section 5.3) handles exactly the syntax above with the fixed standard readtable. The **full reader** (Lisp, `cl/reader.lisp`) implements `read`, `read-from-string`, `read-preserving-whitespace`, `*readtable*`, `copy-readtable`, `set-macro-character`, `get-macro-character`, `set-dispatch-macro-character`, `make-dispatch-macro-character`, `readtable-case`, `*read-base*`, `*read-eval*`, `*read-suppress*`, `*package*`. After M6 the compiler uses the Lisp reader; the C reader stays for bootstrap and is checked against the Lisp reader on a corpus.

### 4.4 Data types

| Type | Notes |
|---|---|
| `null`, `symbol`, `keyword`, `list`, `cons`, `atom`, `boolean` | `NIL` and `T` are static symbols |
| `fixnum`, `integer` | `integer` = fixnum in v1 (31-bit on 32-bit targets, 62-bit on 64-bit). **Overflow signals `pocket:integer-overflow`** (an `arithmetic-error`). Wrapping variants `pocket:fx+ fx- fx*` exist. |
| `float`, `single-float`, `double-float` | **one float representation: IEEE double.** `single-float` and `double-float` are the same type (deviation, 4.15). |
| `ratio`, `complex`, bignums | **not supported.** `/` on integers returns an integer when exact, otherwise signals `pocket:unsupported-feature` ("no ratios; use `(/ (float a) b)` or `floor`"). |
| `character` | Unicode code point (`char-code`, `code-char`); case and `alpha-char-p` etc. cover ASCII and Latin-1 in v1 |
| `string`, `simple-string` | **mutable arrays of characters** (`(setf char)` works). Stored as 32-bit code points in v1 (a narrow 8-bit representation is an optimization the builder MAY add if invisible to Lisp; see section 15) |
| `vector`, `simple-vector`, `array` | rank-1 arrays are first class; adjustable vectors with fill pointers are supported (`make-array :adjustable :fill-pointer`, `vector-push-extend`); rank ≥ 2 arrays are implemented in Lisp over a simple vector; **displaced arrays are not supported** |
| `(simple-array (unsigned-byte 8) (*))` | byte vector. `:element-type` supported: `t`, `fixnum` (= `t`), `character`, `base-char`, `(unsigned-byte 8)`; other element types signal `unsupported-feature` |
| `hash-table` | `:test` `eq eql equal equalp`, insertion-ordered (conformant: CL leaves order unspecified), `gethash` returns two values |
| structures | `defstruct`; instances are kernel records |
| conditions | `define-condition`, implemented in Lisp (4.12); **not CLOS classes** |
| `function`, `compiled-function` | closures and primitives |

### 4.5 Special operators

**Supported (compiler-native):** `quote function if setq progn let let* flet labels block return-from tagbody go catch throw unwind-protect multiple-value-call multiple-value-prog1 the locally macrolet symbol-macrolet eval-when`. `declare` is processed in all binding forms.

**Not supported:** `load-time-value`, `progv`.

`the` is accepted and treated as identity in v1 (a checked mode under `(safety 3)` is optional). `declare` and `declaim` process `special`, `inline`, `notinline`, `ignore`, `ignorable`, `dynamic-extent` (ignored), `type` and `ftype` (parsed, ignored in v1), and `optimize` (accepted, ignored in v1).

### 4.6 Lambda lists

All standard lambda-list keywords are supported in `lambda`, `defun`, `defmacro`, `flet`, `labels`, `destructuring-bind`: `&optional` (with defaults and supplied-p), `&rest`, `&key` (with `&allow-other-keys` and supplied-p), `&aux`, and in macros `&body`, `&whole`, `&environment` and destructuring.

**Compiler-core lambda lists** (what the VM and stage 0 understand) are **required parameters plus `&rest` only**. A shared Lisp function (`boot/10-lambda-list.lisp`) expands every richer lambda list into a core one. It must be written in the restricted boot subset (no `&key`) so that stage 0 can run it. Native VM support for `&optional`/`&key` is a Rule-of-Promotion candidate (M12), not a v1 requirement.

### 4.7 Macros and compilation

- `defmacro` (with destructuring and `&environment`), `macrolet`, `define-symbol-macro`, `symbol-macrolet`, `define-compiler-macro`, `macroexpand`, `macroexpand-1`, `macro-function`, `compiler-macro-function`, `gensym`, `gentemp`, `eval-when`.
- Macros are **non-hygienic**, exactly as in CL: use `gensym` for introduced variables. Lisp-2 already prevents most function-name capture.
- **Build model (a deliberate superset of ANSI `compile-file`):** `pocket build` processes each top-level form in order by **compiling and loading it into the compiler image, and emitting it to the fasl**. Earlier `defun`s, `defmacro`s, `defstruct`s, `defvar`s, `defpackage`s, and `in-package`s are therefore available to the macros of later forms without `eval-when` ceremony. Code with correct `eval-when` stays valid.
- `eval`, `compile`, `compile-file`, `load` (source), and `disassemble` exist **only in the `dev` profile**, which includes the compiler. In the `runtime` profile they signal `pocket:unsupported-feature`.

### 4.8 Packages

A reduced but faithful package system: `defpackage` (`:use :export :import-from :shadow :shadowing-import-from :nicknames :documentation`), `in-package`, `make-package`, `find-package`, `package-name`, `intern`, `find-symbol`, `export`, `unexport`, `import`, `shadow`, `use-package`, `unuse-package`, `symbol-package`, `do-symbols`, `do-external-symbols`, `*package*`, qualified names (`pkg:sym`, `pkg::sym`), `keyword`. Iteration over a package is **insertion-ordered**. `rename-package`, `delete-package`, and `do-all-symbols` are excluded in v1.

Standard packages: `COMMON-LISP` (nickname `CL`), `COMMON-LISP-USER` (`CL-USER`), `KEYWORD`, `POCKET` (extensions, 4.16), `POCKET.SYS` (kernel hooks, `%` primitives, internal). Project packages: `POCKET.COMPILER`, `POCKET.TEST`, `POCKET.UI`, `POCKET.RUNTIME`, `POCKET.CLI`, and so on.

### 4.9 Special variables and dynamic extent

`defvar`, `defparameter`, `defconstant`, `declaim special`, dynamic `let` / `let*` bindings, `boundp`, `symbol-value`, `makunbound`. Implemented by the VM with shallow binding: a dynamic-state stack records `(symbol, old value)`; non-local exits restore bindings. Standard variables supported: `*package* *readtable* *read-base* *read-eval* *print-escape* *print-readably* *print-base* *print-radix* *print-length* *print-level* *standard-output* *standard-input* *error-output* *terminal-io* *features* *random-state* *gensym-counter* *macroexpand-hook* *debugger-hook*`.

### 4.10 Multiple values

`values`, `values-list`, `multiple-value-bind`, `multiple-value-list`, `multiple-value-call`, `multiple-value-prog1`, `multiple-value-setq`, `nth-value`. `multiple-values-limit` = 64. A function returns one value by default; extra values are carried in a VM side buffer with a count register, and the **single-value return path resets the count** so single-value code pays one store. Standard functions return the standard multiple values (`floor`, `truncate`, `gethash`, `read-from-string`, ...).

### 4.11 Non-local exits

`block`/`return-from`, `tagbody`/`go`, `catch`/`throw`, `unwind-protect` share one mechanism: **a dynamic-state stack of entries** (catch tags, block escapes, unwind cleanups, special bindings, handler clusters). Non-local exit unwinds that stack, running cleanups, in order. `block`/`go` that do not cross a closure boundary compile to plain jumps. Exits have **dynamic extent only** (CL semantics): using a dead exit signals `control-error`.

### 4.12 Conditions and restarts (in Lisp)

Supported: `define-condition` (`:report`, `:reader`, `:initarg`, `:initform`, multiple supertypes), `make-condition`, `signal`, `error`, `cerror`, `warn`, `handler-case`, `handler-bind`, `ignore-errors`, `restart-case`, `restart-bind`, `with-simple-restart`, `with-condition-restarts` (basic), `invoke-restart`, `find-restart`, `compute-restarts`, `abort`, `continue`, `muffle-warning`, `use-value`, `store-value`, `assert`, `check-type`, `ecase`/`etypecase` errors, and the standard condition types (`condition warning serious-condition error simple-error simple-condition simple-warning type-error cell-error unbound-variable undefined-function arithmetic-error division-by-zero program-error control-error stream-error end-of-file file-error parse-error`), plus `pocket:unsupported-feature`, `pocket:integer-overflow`, `pocket:heap-exhausted`, `pocket:stack-overflow`.

Handler clusters live in a special variable bound with `let`. **Handlers run at the signal point, before unwinding** (standard CL semantics).

**Kernel errors become Lisp conditions without C unwinding.** When a primitive or VM instruction fails, the C code requests a signal; the VM then performs an ordinary Lisp call to the hook `pocket.sys::%signal-error` (code and arguments), so `handler-bind` handlers and restarts see the original stack. Before the hook is installed (early boot), the kernel prints a debug message and aborts.

An uncaught error in the CLI prints the condition report and a Lisp backtrace (procedure names) and exits non-zero. A frame that signals an unhandled error is logged and skipped; after three consecutive failing frames the app aborts.

### 4.13 `setf`, structures, and types

- **`setf`:** places supported: variables and symbol macros, `car cdr first..tenth rest nth nthcdr elt aref svref char schar gethash get getf symbol-value symbol-plist subseq fill-pointer values` (basic), structure accessors, plus user places via `defsetf` (short and long form), `define-setf-expander`, `get-setf-expansion`, and `(defun (setf name) ...)`. Also `psetf shiftf rotatef incf decf push pop pushnew remf define-modify-macro`.
- **`defstruct`:** options `:conc-name :constructor` (including BOA lists) `:copier :predicate :include :print-function :print-object :type` (only `vector`/`list` untyped forms optional), slot options `:type` (ignored) and `:read-only`. Single inheritance via `:include`. Instances print as `#S(NAME :SLOT value ...)`.
- **Types:** `typep`, `typecase`, `etypecase`, `check-type`, `deftype`, `type-of`, `coerce` (to `list vector string character float`), basic `subtypep`. Type specifiers: the built-in types in 4.4, `(or ...)`, `(and ...)`, `(not ...)`, `(member ...)`, `(eql ...)`, `(satisfies ...)`, `(integer lo hi)`, `(simple-array ...)`, `(vector ...)`, structure and condition names.

### 4.14 Library surface

Appendix A lists coverage by CLHS chapter. Priority tiers (used to order the work and to defer rare features):

- **Tier A (needed to build the system itself):** control and data-flow macros, `defstruct`, `setf`, sequences and lists basics, strings, hash tables, `format` basics, conditions.
- **Tier B (common application code):** the rest of lists and sequences, `loop`, `format` directive subset, string streams, `random`, `sort`, arrays, characters.
- **Tier C (rare):** `loop` corner cases, `format` exotica, `with-hash-table-iterator`, set operations with `:test-not`, `tree-equal`, `sublis`.

**`loop`:** the full extended syntax for: `named initially finally with for/as (in on across from to upto below downto above downfrom upfrom by = then being the elements/hash-keys/hash-values/symbols) repeat while until always never thereis collect append nconc sum count maximize minimize into do when unless if else end return`. Destructuring in `for` and `with` is supported. `loop` is a Lisp macro.

**`format`:** directives `~A ~S ~D ~B ~O ~X ~F ~C ~% ~& ~~ ~T ~* ~P ~( ~) ~{ ~} ~[ ~] ~^ ~; ~newline` with common parameters and modifiers. A **compiler macro** compiles constant control strings to direct calls at build time. Unsupported directives are rejected at build time for literal strings.

### 4.15 Exclusions and deviations (all must be documented and fail loudly)

**Excluded entirely (not exported):** CLOS and MOP (`defclass defgeneric defmethod make-instance slot-value class-of find-class print-object` as a generic, `with-slots`, `change-class`, and so on), `load-time-value`, `progv`, pathnames (`pathname`, `merge-pathnames`, `directory`, logical pathnames; file names are strings), `pprint` and `pprint-logical-block` and `*print-pretty*` (always `NIL`), `rename-package`, `delete-package`, `read-sequence` on binary streams, `broadcast`/`concatenated`/`two-way`/`echo` streams, `step`, `inspect`, `ed`, `room`, `require`, `provide`, `get-decoded-time`, `sleep` (wall-clock; excluded from the app profile).

**Deviations (documented, loud where possible):**

1. No bignums, ratios, or complex numbers (4.4). `integer` is fixnum-sized; overflow signals.
2. One float type (IEEE double); `single-float` and `double-float` are aliases. Float printing is shortest round-trip, with `d0` suffix omitted when the float format is the default.
3. `time`, `get-internal-real-time`, `get-universal-time`, `describe`, `documentation`, `disassemble`, `trace` exist only in the `dev` or tool profile. Wall-clock functions are never available to the frame loop.
4. Proper tail calls are guaranteed (stronger than ANSI).
5. The build model processes top-level forms by compile-and-load (4.7), a superset of ANSI `compile-file`.
6. Hash tables and packages iterate in insertion order (allowed: order is unspecified in the CLHS).
7. Symbol and string text is UTF-8 in sources and fasls and decoded to 32-bit characters in memory. Case conversion covers ASCII and Latin-1 in v1.
8. `declare type`, `the`, and `optimize` are accepted but do not change behavior in v1.

### 4.16 Extensions (`POCKET` package)

Never shadowing `COMMON-LISP`. Required in v1:

- `pocket:unsupported-feature`, `pocket:integer-overflow`, `pocket:heap-exhausted`, `pocket:stack-overflow` (condition types)
- `pocket:fx+ fx- fx*` (wrapping fixnum arithmetic), `pocket:u16-ref u32-ref i16-ref i32-ref` and their `setf`s on byte vectors (little-endian), `pocket:bytes-fill`, `pocket:bytes-replace`
- `pocket:gc`, `pocket:gc-stats`
- `pocket:now-frames`, `pocket:after`, `pocket:every` (frame clock; section 8.1)

---

## 5. The C kernel

The kernel SHOULD stay under roughly **12k lines of C** (excluding `platform/`, tests, and vendored code). It compiles cleanly with `-std=c99 -Wall -Wextra -Wpedantic -Werror` on gcc and clang.

### 5.1 Value representation

`typedef uintptr_t Value;` (word-size tagged value)

| Low bits | Meaning |
|---|---|
| `...1` | fixnum (`value >> 1`) |
| `..000` | heap pointer (8-byte aligned), including the static objects `NIL` and `T` |
| `..010` | immediate: character, unbound marker, unsupplied marker (for `&optional`/`&key` supplied-p), eof marker |

The builder MAY adjust the exact tag layout if the invariants hold (fixnums immediate, heap objects 8-aligned, `NIL` is a distinct static symbol testable with one compare) and the change is recorded.

Heap object header (one word): `type:8 | mark:1 | flags | size`. Types: `CONS VECTOR STRING BYTES ARRAY_HEADER SYMBOL PACKAGE FLONUM CLOSURE PROTO PRIMITIVE HASHTABLE RECORD RECORD_TYPE BOX`.

**Symbols** hold: name, home package, value cell, function cell, property list, flags (`special`, `constant`, `macro`, `special-operator`, `external`). Global functions are called through the function cell, so redefinition works and the VM needs one call instruction for global calls.

### 5.2 Heap and GC

- **One arena**, allocated at startup (defaults: 16 MB desktop, 4 MB runtime-only constrained). No `malloc` after init.
- **Stop-the-world, non-moving mark-sweep**, with size-segregated free lists for small objects and a first-fit list for large ones.
- **GC runs only at VM safe points.** Before any instruction that may allocate (cons, closure creation, primitive call), the VM checks `bytes_allocated_since_gc > threshold` and collects **first**, while all live values are on the VM stack, in globals, or in the symbol and package tables. Primitives never trigger GC and need no root-protection macros. They allocate from a guaranteed reserve (`heap_reserve`, default 64 KB); a primitive needing more (for example a huge `make-array`) is checked up front and signals `pocket:heap-exhausted`.
- **Roots:** VM value stack, dynamic-state stack, multiple-values buffer, all symbols (strong in v1; symbols reachable only from the table are kept) and packages, the registered frame function, the host event queue buffer.
- `(pocket:gc)` and `(pocket:gc-stats)`. The frame loop MAY collect at frame end when the heap is above a soft threshold.
- **GC stress mode** (`-DPOCKET_GC_STRESS` or a runtime flag): collect before every allocating instruction. All tests MUST also pass under stress mode.

### 5.3 Bootstrap reader and debug printer

The **bootstrap reader** implements the syntax in 4.3 with the fixed standard readtable, `:upcase` case, package-qualified symbols (it calls `intern` against `*package*`, read from the `*package*` symbol's value cell), `#+`/`#-` using `*features*`, and **no** user reader macros. It MUST NOT crash on any input (fuzz-tested under ASAN) and returns structured errors with line and column. It MAY record source positions for list forms in a side table.

The **debug printer** prints any object in a stable, readable form (`#<FUNCTION NAME>`, `#S(...)`, `#<HASH-TABLE ...>`) for boot, crash reports, and tests of the kernel. The CL printer (`prin1`, `princ`, `print`, `write`, `format`) is Lisp (section 7.1).

### 5.4 Stage-0 evaluator

A tree-walking evaluator (SHOULD be under about 2,000 lines) that supports the compiler-native special operators of 4.5 that the boot code needs (`quote function if setq progn let let* flet labels block return-from tagbody go catch throw unwind-protect multiple-value-call multiple-value-prog1 the locally`), compiler-core lambda lists (required plus `&rest`), special-variable binding, multiple values, **proper tail calls** (trampoline), and on-the-fly macro expansion of macros defined by the primitive `%defmacro`. Non-local exits use the same dynamic-state design as the VM (a `setjmp`-based escape stack is acceptable here). Its only job is to run the Lisp compiler once so that the compiler can compile itself. It is compiled out of runtime images (`-DPOCKET_NO_EVAL0`). Speed is irrelevant. Correctness is tested by the shared conformance suite (section 11).

### 5.5 Bytecode VM

Stack machine with flat closures. Captured variables are copied into the closure; variables both captured and assigned (`setq`) are boxed by the compiler. Operand encoding is up to the builder (document in `docs/bytecode.md`). Required instruction groups:

- **Constants and variables:** `CONST`, `LOCAL`, `SETLOCAL`, `FREE`, `GLOBAL` (special/global value cell, with unbound check), `SETGLOBAL`, `POP`, `DUP`
- **Control:** `JUMP`, `JUMPIFNIL`, `JUMPIFNOTNIL`, `CALLGLOBAL sym n` (function cell, with undefined-function check), `FUNCALL n`, `TAILCALLGLOBAL`, `TAILFUNCALL`, `RETURN`, `APPLY`
- **Closures:** `CLOSURE proto nfree`, `BOX`, `UNBOX`, `SETBOX`
- **Rest args:** `MAKEREST n` (collect extra arguments into a list)
- **Multiple values:** `VALUES n`, `MVCALL`, `MVBIND`, `MVPROG1`, with the count register of 4.10
- **Dynamic state:** `BINDSPECIAL sym`, `UNBINDSPECIAL`, `PUSHCATCH`, `PUSHBLOCK`, `PUSHUNWIND`, `POPDYN`, `THROW`, `RETURNFROM`, `GO` (non-local forms), `HANDLER` clusters are ordinary special-variable bindings (4.12)
- **Inlined hot primitives** (emitted when the callee is an unshadowed standard function and the compiler can prove it): `CAR CDR CONS RPLACA RPLACD NULL CONSP EQ EQL NOT 1+ 1- ADD SUB MUL LT LE EQNUM SVREF SCHAR CHAR-CODE RECORD-REF` and similar

**Inline-op slow-path protocol.** Inline ops have fast paths for the common types (fixnum and flonum arithmetic, simple vectors and strings, conses). On any miss the VM performs an ordinary Lisp call to a fallback global function in `POCKET.SYS` (for example `%add-slow`, `%aref-slow`, `%car-error`). Everything beyond the fast path (type errors, adjustable arrays, array headers, overflow signaling, and later bignums) is therefore **Lisp**, and no C primitive ever calls back into Lisp.

The VM has a fixed-size value stack (default 256K slots; overflow signals `pocket:stack-overflow`, never a crash), a call-frame stack, the dynamic-state stack, and a per-procedure name for backtraces. A sampling hook (periodic counter calling a registered Lisp function) supports the profiler (M12).

### 5.6 Primitive set (C)

Keep this list as small as possible. Everything derived (`mapcar`, `remove-if`, `sort`, `string-upcase`, `format`, `loop`, `gethash` wrappers, `maphash`, `with-output-to-string`, and so on) is Lisp. Names in `POCKET.SYS` start with `%`; where a standard name is the primitive itself, the `COMMON-LISP` symbol is bound directly.

- **Symbols and packages:** `symbol-name`, `symbol-package`, `symbol-value`/`set`, `symbol-function`/`%set-symbol-function`, `symbol-plist`/`%set-symbol-plist`, `boundp`, `fboundp`, `makunbound`, `fmakunbound`, `make-symbol`, `intern`, `find-symbol`, `%make-package`, `find-package`, `%package-name`, `%export`, `%unexport`, `%import`, `%shadow`, `%use-package`, `%package-next` (iteration cursor).
- **Conses and identity:** `cons car cdr rplaca rplacd eq eql`; `consp atom null listp` (inline); `c[ad]{2,4}r` are compiler-expanded.
- **Numbers:** generic `+ - *` on fixnum and flonum (mixed allowed), `%truncate %floor %ceiling %round` (two values), `rem mod`, `= /= < <= > >=`, `float`, `ash logand logior logxor lognot logbitp`, `sqrt exp log sin cos tan atan` (two-arg `atan`), `expt` for float or small-integer cases, `%flonum->string` (shortest round-trip), `%parse-flonum`. Everything else (`gcd lcm isqrt min max abs signum random ...`) is Lisp.
- **Characters:** `char-code`, `code-char` (immediates).
- **Simple vectors:** `%make-simple-vector`, `svref`, `(setf svref)`, `%simple-vector-length`, `%replace-simple-vector` (overlap-safe range copy).
- **Strings:** `%make-string n char`, `schar`, `(setf schar)`, `%string-length`, `%replace-string`, `%string-hash`.
- **Byte vectors:** `%make-bytes`, `%bytes-length`, u8 ref/set, little-endian `u16/u32/i16/i32` ref/set, `%replace-bytes`, `%fill-bytes`.
- **Array headers** (adjustable vectors with fill pointers): `%make-array-header data fill-pointer flags` and accessors/setters.
- **Hash tables:** `%make-hash-table test size`, `%table-ref` (returns value and found-p as two values), `%table-set`, `%table-remove`, `%table-clear`, `%table-count`, `%table-next` (insertion-ordered cursor). Content-based hashing for strings, symbols, numbers, characters; `equal`/`equalp` hashing uses a Lisp-supplied structural walk only through content (addresses are never visible to iteration order).
- **Records:** `%make-record-type`, `%make-record`, `%record-ref`, `%record-set`, `%record-type-of`, `%record-p`.
- **Control hooks:** `%install-hooks` (registers the Lisp error hook and slow-path fallbacks), `funcall`, `apply`, `values` (VM-level).
- **I/O:** `%console-write-string`, `%console-read-line` (desktop), `%read-file` (byte vector), `%write-file` (tool profile only).
- **Images:** `%load-fasl` (from a byte vector), `%fasl-entry`.
- **GC:** `pocket:gc`, `pocket:gc-stats`.
- **Host (section 5.9):** `host-*` primitives.
- **Tool profile only:** `%host-time-ms` (benchmarking and pacing).

### 5.7 Fasl format (`.fasl`)

Little-endian, no timestamps, no pointers, **fully deterministic** (required for the bootstrap fixed point; gensym counters are scoped per compilation unit, package and symbol iteration is insertion-ordered).

```
magic "PFAS" | u16 version | u16 flags (bit0: contains compiler, bit1: debug names)
symbols   : home package name + symbol name (UTF-8); uninterned symbols by per-image index
constants : fixnum, flonum, string, character, symbol ref, cons/list, simple vector, byte vector
protos    : arity (required count, rest?), nfree, name symbol ref, code bytes, const ref list, optional line table
toplevel  : sequence of top-level procedures run at load, in order
```

The loader finds or creates packages by name (a later `defpackage` form completes them), interns symbols in their home package, and validates all bounds, failing with an error (never UB) on a malformed image. Constants are coalesced by `equal` where CL allows.

### 5.8 Error handling in C

C code never unwinds the Lisp stack. A failing primitive or instruction **requests a signal** (error code plus datum arguments); the VM then calls `pocket.sys::%signal-error` as an ordinary Lisp call (4.12). The only C unwinding is inside stage 0 and for fatal errors (heap corruption, hook missing), which print a debug message and abort.

### 5.9 Platform ABI (`kernel/include/platform.h`)

A struct of function pointers; a backend fills it in.

```c
typedef struct PocketPlatform {
  int   (*init)(const PlatformConfig *cfg);        // window/fb setup; cfg: width, height, scale
  void  (*shutdown)(void);
  bool  (*poll)(InputBlock *out);                  // fill input; return false on quit
  void  (*upload)(int kind, int id, const uint8_t *data, int w, int h, int fmt); // fonts/images
  void  (*present)(const uint8_t *drawlist, size_t len); // rasterize + show
  void  (*pace)(void);                             // sleep to 60 Hz; headless: no-op
  bool  (*next_event)(HostEvent *out);             // native-module events (net/audio later)
  int64_t (*read_asset)(const char *name, uint8_t *buf, size_t cap); // optional
} PocketPlatform;
```

`InputBlock` (12 bytes, shared with Lisp as a preallocated byte vector so no allocation per frame): `u32 buttons` (level bitmask), `i16 pointer_x`, `i16 pointer_y`, `u8 pointer_down`, `u8 reserved[3]`.

Button bits: `UP DOWN LEFT RIGHT A B X Y L R START SELECT` (A = confirm, B = back).

`main()` is about 40 lines of C: parse args, init platform, init VM and packages, load the image, run its top-level forms, look up the function `POCKET.RUNTIME::FRAME`, then:

```c
while (platform->poll(&input)) { vm_call(vm, frame_fn); platform->pace(); }
```

The Lisp primitives `host-upload`, `host-present`, `host-input-block`, `host-next-event`, and `host-quit` (package `POCKET.SYS`) expose this ABI. `present` is called from Lisp **only when something is dirty**.

---

## 6. The Lisp compiler and the bootstrap

### 6.1 Compiler pipeline (all Lisp, `lisp/compiler/`)

1. **Read** (C bootstrap reader during bootstrap, the Lisp reader afterwards).
2. **Expand:** `macroexpand-all` with a compile-time environment that has **separate variable and function namespaces**, local macros (`macrolet`), symbol macros (`symbol-macrolet`, `define-symbol-macro`), special-variable proclamations, inline declarations, and `&environment` passed to macros. `setf` places, `loop`, `defstruct`, `handler-case`, and the rest reduce to compiler-native operators. Compiler macros are applied (and can be disabled by `notinline`).
3. **Analyze:** produce an AST of records (`const`, `local-ref`, `special-ref`, `set`, `if`, `lambda`, `call`, `global-call`, `funcall`, `seq`, `block`, `tagbody`, `mv-*`, `dyn-bind`, and so on); compute free variables, assigned-and-captured sets, tail positions, and which exits cross closures.
4. **Optimize** (simple, safe, deterministic): constant folding, dead-branch removal, `let`-to-lambda collapse, known-function inlining of standard primitives, user `inline` expansion for small non-recursive `defun`s declaimed inline, `format` and `typep` compiler macros.
5. **Closure conversion** (flat closures, boxing assigned captured variables).
6. **Codegen** to the bytecode assembler (labels, jump resolution).
7. **Fasl writer and linker:** resolve system dependencies, process packages, **tree-shake** unreachable definitions (reachability from the entry points and exports; any symbol appearing as a quoted constant keeps its function definition, conservatively), deduplicate constants, emit `.fasl` deterministically.

Errors carry file, line, column and the macro-expansion chain, and name excluded standard symbols with a pointer to `docs/conformance.md`.

### 6.2 Bootstrap sequence

```
Stage 0: build pocketvm0 (kernel with eval0)
         pocketvm0 loads lisp/boot/*.lisp then lisp/compiler/*.lisp
         → compile the compiler sources → build/stage1/compiler.fasl

Stage 1: pocketvm (bytecode VM, no eval0) runs build/stage1/compiler.fasl
         → compile the compiler sources again → build/stage2/compiler.fasl

Check:   stage1/compiler.fasl and stage2/compiler.fasl MUST be byte-identical.
```

Rules:

- `lisp/boot/00-core.lisp` MUST use **only** compiler-native operators, required/`&rest` lambda lists, and primitive calls. It defines `defmacro` (via `%defmacro`), `defun`, `lambda`, `let`-family helpers, `cond`, `and`, `or`, `when`, `unless`, backquote expansion, `defpackage`, `in-package`, `defvar`, `defparameter`, using only `list`, `cons`, `append`.
- It is read with `*package*` bound to `COMMON-LISP` until `defpackage` exists; later boot files declare their own packages.
- `lisp/boot/10-lambda-list.lisp` implements the shared lambda-list expander (4.6) in the same restricted subset.
- Everything after boot may use the full set of derived forms as they come online (`setf`, `defstruct`, `loop`, conditions are layered in M4).
- The compiler sources MUST NOT depend on stage-0-only behavior. The shared conformance suite runs on both stage 0 and the VM.
- `make bootstrap` runs the whole chain and the fixed-point check. CI runs it on every change to `kernel/` or `lisp/compiler/`, `lisp/boot/`, or `lisp/cl/`.

### 6.3 Binaries

- `pocketvm0`: stage-0 bootstrap binary (has eval0).
- `pocketvm`: runtime kernel (VM and loader, optionally with the compiler image).
- `pocket`: the user-facing CLI (`pocketvm` plus the CLI image; a wrapper script is fine).

---

## 7. Lisp library, systems, and tooling

### 7.1 The Common Lisp library (Lisp, `lisp/cl/`)

All of the following is written in PocketCL on top of section 5.6 primitives. Package declarations first (`cl/package.lisp`), then in dependency order:

- **Language layer:** `lambda-list`, `setf` machinery, `defstruct`, `typep`/`deftype`/`check-type`, control macros (`do do* dolist dotimes case ecase typecase etypecase prog prog* destructuring-bind psetq ...`), `loop`.
- **Conditions:** the condition system and restarts of 4.12, and the standard error constructors used by `%signal-error`.
- **Data:** lists (full chapter 14), sequences (chapter 17, with `:key :test :test-not :start :end :from-end :count`), strings and characters (chapters 13 and 16), numbers above the primitives (chapter 12: `floor round gcd lcm expt isqrt min max abs signum random`, `random-state` deterministic by seed), arrays (chapter 15: `make-array`, `aref`, `vector-push-extend`, `adjust-array`, rank ≥ 2 over simple vectors), hash-table API (`maphash`, `with-hash-table-iterator` Tier C, `remhash`, `clrhash`, `hash-table-count`, `loop` support), symbols and `get`/`getf`, packages API.
- **I/O:** streams (string input and output streams, console streams, file streams in the tool profile), the **full printer** (`prin1 princ print write write-string write-line write-char terpri fresh-line`, `*print-escape* *print-readably* *print-base* *print-radix* *print-length* *print-level*`, structure printing, no pretty printer), the **full reader** (4.3), `format`, `with-output-to-string`, `with-input-from-string`, `read-line`, `read-char`, `peek-char`, `unread-char`.
- **Environment:** `lisp-implementation-type`, `lisp-implementation-version`, `machine-type` (constants), and tool-profile `time`.

### 7.2 Test framework (`pocket.test`, Lisp)

```lisp
(deftest append-works ()
  (is (equal '(1 2 3 4) (append '(1 2) '(3 4))))
  (signals type-error (car 5)))
```

`deftest is signals is-equal` (plus `finishes`, `skip`). `pocket test [paths]` discovers `*-test.lisp`, runs each test isolated with `handler-case`, prints a summary, and exits non-zero on failure. Supports `--stress` (GC stress) and `--filter`. The harness is portable (a small shim in `tests/conformance/portable-harness.lisp` that also loads under SBCL), so conformance tests can run in both.

### 7.3 Systems (`defsystem`-lite, Lisp)

```lisp
(defsystem :pocket.ui
  :depends-on (:pocket.cl)
  :serial t
  :components ("package" "signals" "arena" "style" "layout" "text" "draw" "input" "focus" "runtime" "components"))
```

Systems live under `systems/` and in `lisp/**/system.lisp`. `pocket build` loads a system and its dependencies in order using the build model of 4.7. This is not ASDF and does not aim to be.

### 7.4 CLI (`pocket`, Lisp)

| Command | Behavior |
|---|---|
| `pocket build <system> -o app.fasl [--profile runtime\|dev] [--strict]` | compile and link |
| `pocket run <system>\|app.fasl [--headless] [--window] [--tape f] [--hash] [--chaos]` | run |
| `pocket test` | run Lisp tests |
| `pocket repl` | REPL (desktop) with `inspect-tree`, `dump-drawlist`, `macroexpand`, `disassemble` |
| `pocket check` | static checks: unbound functions and variables, excluded CL symbols, arity of known functions, unused imports |
| `pocket bench` | run `bench/` against budgets |
| `pocket conformance` | regenerate and verify `docs/conformance.md` against the symbol list |

**Profiles:** `dev` includes the compiler, `eval`, REPL support, docstrings, and debug names. `runtime` strips them (no compiler, no eval0, no debug names, no docstrings), producing the smallest image.

---

## 8. The UI runtime (all Lisp, `lisp/ui/`)

### 8.1 Frame model (the heart of determinism)

State advances as `state[n+1] = F(state[n], input[n])` and `pixels[n] = G(state[n])`. The C loop calls Lisp `pocket.runtime::frame` once per frame. Order inside a frame is fixed:

1. Increment the frame counter.
2. Drain the host event queue (FIFO) and deliver callbacks (`host-next-event`).
3. Run `after` timers whose deadline is at or before the current frame, in registration order.
4. Read the input block; derive pressed and released edges; update focus; dispatch `:on-press` and other handlers.
5. Flush the reactive graph (effects run).
6. If anything is dirty: resolve styles, lay out dirty subtrees, rebuild the drawlist, call `host-present`. Otherwise return without presenting.

Rules:

- **Time is frames.** `(pocket:now-frames)`, `(pocket:after n fn)` (n frames from now; returns a cancel handle), `(pocket:every n fn)`. Lisp code cannot read wall-clock time in the app profile.
- **Async results enter between frames.** An event that arrives mid-frame is queued and delivered at the start of the next frame (step 2).
- **Randomness** comes from a seeded `random-state` held in runtime state (seed settable by the host, recorded in tapes).
- **Record and replay:** a *tape* is a text file, one line per frame: `<buttons-hex> [<px> <py> <down>] [xN]` (the `xN` suffix repeats the line N frames), with `#` comments. `pocket run --tape f --headless --hash` prints one framebuffer hash per frame.
- **Chaos mode** (`--chaos`, headless only): injects random host sleeps, forces GC between frames, and perturbs allocation patterns. The recorded hash sequence MUST NOT change.

### 8.2 Reactivity (fine-grained, no VDOM; CL-idiomatic)

The current observer is held in a special variable (`*observer*`), and signals can be bound to **symbol macros** so ordinary Lisp variable syntax reads and writes them.

| Form | Meaning |
|---|---|
| `(make-signal v)` | create a signal object |
| `(signal-value s)` | read; **tracks** a dependency when inside an effect or memo. `(setf (signal-value s) v)` writes. |
| `(let-signals ((n 0) (name "x")) body...)` | binds each name as a **symbol macro** over a fresh signal: `n` reads (tracking), `(setf n 5)`, `(incf n)` write |
| `(defsignal *n* 0)` | global signal bound with `define-symbol-macro` |
| `(memo expr...)` / `(make-memo fn)` / `(memo-value m)` | derived value |
| `(effect expr...)` / `(make-effect fn)` | side effect re-run when dependencies change |
| `(batch body...)` | defer propagation to the end of the body |
| `(untracked body...)` | read without tracking |
| `(on-cleanup fn)` | runs before the owning effect re-runs or is disposed |
| `(with-root (dispose) body...)` | ownership root; binds `dispose` to a function |

Writes of an `eql`-identical value are no-ops (record in the decision log if `equal` is preferred).

Algorithm: SHOULD use the three-state coloring approach (CLEAN, CHECK, DIRTY) used by Reactively and Solid for glitch-free propagation. Owners form a tree; disposing an owner disposes its child effects and runs cleanups in reverse creation order.

Required properties (each has a test): diamond dependencies run the downstream effect once; dynamic dependencies are re-collected per run; effects created inside effects are owned and disposed correctly; `batch` coalesces; no subscription leaks after dispose (test by counting live subscriptions); effect execution order is deterministic; `*observer*` is restored correctly on non-local exits out of an effect.

### 8.3 Components and UI macros

Components are plain Lisp functions that run **once** and wire reactive bindings, like Solid.

```lisp
(view  :class "..." :focusable t :on-press fn  child...)   ; container
(text  :class "..."  child)                                 ; text leaf; child is a string or expression
(image :class "..." :src id)                                ; M13
(show  test-form then-form [else-form])                     ; conditional subtree
(each  (item list-form :key key-form [:index i]) body...)   ; keyed list
(mount function)                                            ; set the root component
```

Props are a keyword/value prefix before the children (the macro consumes `keyword value` pairs while the next argument is a keyword). Props: `:class :focusable :on-press :on-focus :on-blur :id`.

**Macro rule for reactivity:** in a prop value or child position, any form that is not a self-evaluating literal, a `quote`, or a `function`/`lambda` form is compiled into an *effect* that updates the node when its signal dependencies change. Handler props (names beginning `:on-`) are never wrapped. The macro SHOULD use `macroexpand-all` with its `&environment` to skip effects around forms that provably read no signals and call no functions (an optimization, not a requirement).

`show` and `each` create owned sub-roots; toggling or removing items disposes their effects. `each` reconciles by key and MUST preserve node identity for retained keys (tested).

### 8.4 Styles: Tailwind-compatible subset, baked at compile time

`:class "px-4 py-2 rounded-xl bg-blue-600 focus:bg-blue-500"` is resolved **at macro-expansion time** (the style module is loaded in the compiler image) into a **quoted literal simple-vector** (`',style-vector`), coalesced across the image by `equal`. At run time a node just stores that constant. A dynamic `:class` expression falls back to a runtime resolver with a string-keyed `equal` hash-table cache and a documented slower path.

Naming and scale follow Tailwind (1 unit = 4 px). Supported v1 subset (document exact slots in `docs/style.md`):

- **Layout:** `flex flex-row flex-col flex-1 grow shrink basis-*`, `items-start|center|end|stretch`, `justify-start|center|end|between|around`, `gap-*`, `w-* h-* min-w-* max-w-* min-h-* max-h-*` (scale, `px`, `full`, fractions, `auto`), `absolute relative inset-* top-* left-* right-* bottom-*`, `overflow-hidden`, `p* m*` (all sides, x, y, t, r, b, l)
- **Visual:** `bg-<color>-<shade>`, `text-<color>-<shade>`, `border`, `border-<n>`, `border-<color>-<shade>`, `rounded rounded-sm|md|lg|xl|full`, `opacity-*` (M13)
- **Text:** `text-xs|sm|base|lg|xl|2xl` (mapped to bitmap font scale in v1), `text-left|center|right`
- **Variants:** `focus:` (overlay applied while focused)
- **Palette:** Tailwind slate, gray, red, orange, amber, emerald, blue, indigo, violet, pink, plus white and black; shade steps 50 to 950.

Unknown classes are a **compile-time warning** (an error under `--strict`).

### 8.5 Layout engine (flexbox subset in Lisp)

- Integer pixel math only (no floats in layout). Rounding rule: round half down, remainders distributed left to right (documented and tested).
- Single-line flex containers in v1 (wrap in M13): `direction`, `grow`, `shrink`, `basis`, `justify-content`, `align-items`, `gap`, padding, margin, border, `min/max` constraints, percentages, `absolute` positioning, `overflow-hidden` clipping.
- **Two passes:** bottom-up *measure* (intrinsic sizes for `auto`), top-down *arrange*.
- **Data layout:** a node is a fixnum id into a struct-of-arrays arena (preallocated simple vectors with a free list): computed `x y w h`, style constant, flags, parent, first-child/next-sibling, text content. This avoids per-frame allocation and GC pressure; geometry lives in flat simple vectors of fixnums. Declare `(optimize (speed 3))` and `inline` helpers in hot paths.
- **Dirty tracking:** `style-dirty`, `layout-dirty` (propagates up to the nearest fixed-size ancestor, a relayout boundary), `paint-dirty`. A frame with no dirty flags does nothing.
- **Tests:** a table of at least 40 hand-verifiable layout cases (input tree, expected rectangles), derived carefully from CSS flexbox semantics, covering grow and shrink distribution, all justify and align modes, gap, padding and margin, min and max, percentages, absolute, nested containers.

### 8.6 Text

v1 font: built-in 8x8 ASCII bitmap font (public-domain `font8x8`) embedded as a byte-vector literal, uploaded to the backend as an atlas via `host-upload`. Text style sizes map to integer scales (1x, 2x, 3x). Characters outside the font render as a fallback glyph. Text layout is greedy word wrap within the node's width, deterministic, with `#\Newline` support. Provide `(measure-text font-id string max-width)` for layout. TTF baking and wider Unicode are M13.

### 8.7 Drawlist (the Lisp to C render contract)

A flat little-endian byte vector of fixed-size records (record size and field layout are the builder's choice; document in `docs/drawlist.md`). Required ops:

`CLEAR rgba`, `RECT x y w h rgba radius`, `BORDER x y w h rgba radius thickness`, `GLYPHS font-id x y rgba count` followed by `count` glyph ids and advances, `IMAGE image-id x y w h`, `CLIP_PUSH x y w h`, `CLIP_POP`, `END`.

The Lisp emitter writes into a preallocated byte vector with `pocket:u32-ref` and friends (grown by doubling when needed, which is rare). The headless backend can also dump the drawlist as stable text, so tests can assert on *what would be drawn* separately from pixel hashes.

### 8.8 Input and focus

- Buttons arrive as a level bitmask; Lisp derives pressed and released edges per frame.
- **Spatial focus navigation:** on D-pad input, move focus to the nearest focusable node in that direction using node center geometry (ties broken by document order). The algorithm is specified and tested in `docs/spec.md`.
- `:on-press` fires on `A` while focused. `:on-focus` and `:on-blur` fire on focus changes. `focus:` style variants apply automatically.
- **Pointer** (mouse or touch): hit-test topmost focusable node; pointer-down focuses it; pointer-up on the same node fires `:on-press`.
- Desktop key mapping (configurable): arrows to D-pad, `Z`/`X`/`A`/`S` to A/B/X/Y, `Q`/`W` to L/R, `Enter` to START, `Right Shift` to SELECT.

### 8.9 Rasterizer and backends (C, shared)

`platform/common/raster.c`: a software rasterizer consuming the drawlist into an RGBA8888 framebuffer (RGB565 variant for constrained targets, compile-time switch).

- **Integer-only**, no floating point anywhere, so output is bit-identical across machines (required for golden tests).
- Rounded rectangles via integer circle-quadrant masks (no anti-aliasing in v1; coverage AA is a later option behind a flag).
- Source-over alpha blending in 8-bit integer math with a fixed rounding rule.
- Clip stack (scissor), 1-bit glyph blit with color, image blit.

Backends only provide window and event glue and a framebuffer blit. `headless` renders into memory, can write PPM, and prints a hash (FNV-1a 64) of the framebuffer.

### 8.10 Animation (M13)

Keyframe timelines and spring curves are expressed with macros and compiled at build time into constant tables; they advance on the **frame clock** (no per-frame allocation, no wall-clock). Animated properties write directly to node arena slots and mark paint-dirty.

### 8.11 Native modules (M15+, opt-in)

Network, audio, and storage are optional C modules exposed as `host-*` primitives plus a Lisp wrapper system. Modules the app does not use are not linked (the tree-shaker removes the Lisp side; the C side is a build flag). Their results arrive through the host event queue (8.1, step 2).

---

## 9. Reference app (`examples/counter`)

```lisp
(defpackage :app.counter
  (:use :cl :pocket.ui)
  (:export #:app))
(in-package :app.counter)

(defun app ()
  (let-signals ((n 0))
    (view :class "w-full h-full flex-col items-center gap-4 p-4 bg-slate-50"
      (text :class "text-xl text-slate-950"
            (format nil "Count: ~d" n))
      (view :class "px-4 py-2 rounded-xl bg-blue-600 focus:bg-blue-500"
            :focusable t
            :on-press (lambda () (incf n))
        (text :class "text-base text-white" "Press A"))
      (show (> n 3)
        (text :class "text-sm text-emerald-600" "Reactive on real hardware.")))))

(mount #'app)
```

Expected: the first frame shows `Count: 0` and a focused button; four A-presses (via tape) show `Count: 4` and the green line; the green line's effects are disposed when `show` turns false. Headless golden hashes for frames 0, 1, 5 are committed. The `format` call is compiled to direct code at build time, and the `:class` strings are baked into constants.

---

## 10. Repository layout

```
pocketcl/
├── roadmap.md                  (this file)
├── README.md
├── Makefile
├── docs/
│   ├── spec.md  conformance.md  bytecode.md  drawlist.md  platform.md  style.md  decisions.md  perf.md
├── kernel/                     (C99)
│   ├── include/  pocket.h  value.h  platform.h  vm.h
│   └── src/  value.c  heap.c  gc.c  symbol.c  package.c  reader0.c  debugprint.c  eval0.c
│             vm.c  loader.c  prim_*.c  host.c  main.c
├── platform/
│   ├── common/  raster.c  raster.h  fb_hash.c
│   ├── headless/  sdl2/  wasm/  <device>/
├── lisp/
│   ├── boot/        00-core.lisp  10-lambda-list.lisp  20-cl-macros.lisp
│   ├── compiler/    env.lisp expand.lisp analyze.lisp optimize.lisp closure.lisp codegen.lisp
│   │                fasl.lisp link.lisp system.lisp
│   ├── cl/          package.lisp conditions.lisp restarts.lisp types.lisp setf.lisp defstruct.lisp
│   │                loop.lisp control.lisp lists.lisp sequences.lisp strings.lisp chars.lisp
│   │                numbers.lisp hash.lisp arrays.lisp streams.lisp reader.lisp printer.lisp
│   │                format.lisp packages.lisp random.lisp
│   ├── test/        framework.lisp
│   ├── ui/          package.lisp signals.lisp arena.lisp style.lisp layout.lisp text.lisp
│   │                font-builtin.lisp draw.lisp input.lisp focus.lisp runtime.lisp components.lisp
│   ├── cli/         driver.lisp build.lisp run.lisp repl.lisp check.lisp bench.lisp conformance.lisp
│   └── devtools/
├── systems/
├── tests/
│   ├── c/           (C unit tests, tiny harness)
│   ├── conformance/ (portable CL tests, run on stage 0, the VM, and SBCL if present)
│   ├── lisp/        (*-test.lisp for libraries and UI)
│   ├── golden/      (drawlist text dumps, framebuffer hashes, PPMs)
│   └── tapes/
├── bench/
└── examples/
```

**Makefile targets:** `make` (everything), `make stage0`, `make bootstrap`, `make test`, `make stress` (GC stress), `make asan` (ASAN and UBSAN), `make golden`, `make diff-sbcl` (optional), `make bench`, `make ci` (all of the above except `diff-sbcl` and bench's soft targets).

---

## 11. Testing strategy

| Level | What | Where |
|---|---|---|
| C unit | bootstrap reader round-trip against the debug printer (≥100 cases), heap and GC (cycles, fragmentation, large objects), package and symbol primitives, image loader rejects malformed images, VM instruction tests | `tests/c/` |
| Conformance | language and library semantics, written in the **portable subset** with the portable harness. **Same files run on stage 0 and on the VM; results must match.** Cases are modeled on the CLHS examples (and, where available, the public `ansi-tests` suite). | `tests/conformance/` |
| **Differential (optional)** | if `sbcl` is on the PATH, `make diff-sbcl` runs the same conformance files under SBCL as an oracle. Disagreements are either bugs or documented deviations. | `tests/conformance/` |
| Lisp unit | compiler passes, reader, printer, `loop`, `format`, conditions, reactivity, layout, focus | `tests/lisp/` |
| Reader parity | C bootstrap reader and Lisp reader produce `equal` results on a corpus | `tests/lisp/` |
| Bootstrap | stage 1 and stage 2 compiler images byte-identical | `make bootstrap` |
| Conformance table | `pocket conformance` verifies `docs/conformance.md` lists every standard symbol exactly once and that every "supported" symbol is bound and every "excluded" one is not exported | CI |
| Golden | drawlist text dumps and framebuffer hashes for example apps and widgets | `tests/golden/` |
| Replay | tape → hash sequence; chaos-mode invariance | `tests/tapes/` |
| Stress | the entire test suite under GC stress | `make stress` |
| Sanitizers | ASAN and UBSAN across C tests and the Lisp suites | `make asan` |
| Fuzz | bootstrap reader and fasl loader fed random bytes, must never crash | `tests/c/fuzz_*.c` |
| Parity | native vs WASM hash sequence for the same tape (M11) | `tests/parity/` |

Golden updates require an explicit `make golden-update` and a note in the commit message explaining why the output changed.

---

## 12. Performance and size budgets

Measured on a desktop at `-O2`, single thread, release build. These are **proxies** for a handheld-class CPU (a 333 MHz core has roughly 13x lower clock than a current desktop core, so typical interactions should cost about 1/20 of a 16.7 ms frame on desktop). Report actual numbers in `docs/perf.md` at M12.

| Metric | Target | Gate (CI fails above) |
|---|---|---|
| `(fib 30)` on the VM | ≤ 0.5 s | 1.0 s |
| Special-variable `let` binding plus unbinding | ≤ 0.2 µs | 0.5 µs |
| `handler-case` establishing handlers, no signal | ≤ 3 µs | 10 µs |
| `(format nil "n=~d" n)` with a literal control string | ≤ 5 µs | 15 µs |
| Idle frame (nothing dirty), Lisp time | ≤ 30 µs | 100 µs |
| Counter app, one text change and redraw | ≤ 0.5 ms | 0.8 ms |
| Full relayout, 200 nodes | ≤ 2 ms | 3 ms |
| `each` update of 1,000-item list (one item changed) | ≤ 1 ms | 2 ms |
| Drawlist build, 500 ops | ≤ 1 ms | 2 ms |
| GC pause, 4 MB live heap | ≤ 2 ms | 3 ms |
| Startup to first frame (counter app, runtime profile) | ≤ 40 ms | 70 ms |
| Runtime-profile image, counter app (tree-shaken) | ≤ 384 KB | 512 KB |
| Resident memory, counter app, desktop | ≤ 8 MB | 12 MB |

`make bench` compares against the gates and fails if a metric regresses more than 20% from the recorded baseline.

---

## 13. Milestones

Each milestone lists **Deliverables** and **Acceptance** (all must pass). Do not skip ahead.

### M0: Foundations

**Deliverables:** repo skeleton (section 10), `Makefile`, tiny C test harness (`ASSERT`, test registry), `make ci` (builds and runs C tests with `-Werror`, ASAN, UBSAN), `docs/` stubs, `docs/decisions.md`, and the **list of all standard `COMMON-LISP` symbols** in `docs/conformance.md` (all initially "unimplemented").
**Acceptance:** `make ci` is green with a placeholder test; builds with gcc and clang; the symbol list is complete and machine-checkable.

### M1: Kernel core (values, symbols and packages, heap, GC, bootstrap reader, debug printer)

**Deliverables:** `value.h` tagged representation; arena allocator; all object types; symbols with value and function cells, plists, flags; the package table and its primitives; mark-sweep GC with an explicit root-set API (for tests, before the VM exists); the bootstrap reader (4.3); the debug printer.
**Acceptance:** reader and debug-printer agreement on a ≥100-case corpus (including `'x`, backquote, `#'f`, `#(..)`, characters, qualified symbols, dotted pairs, `#+`/`#-`, nested comments, `:upcase` behavior); package tests (use, export, shadow, import, `find-symbol` second value, conflict errors, insertion-ordered iteration); GC tests (cycles freed, fragmentation survives repeated alloc/free, large object, exhaustion returns an error not a crash); reader fuzz (1M random inputs under ASAN, zero crashes); all under ASAN and UBSAN.

### M2: Stage-0 evaluator, primitives, boot macros

**Deliverables:** `eval0.c`; the primitive set of 5.6 needed by the compiler; special variables and multiple values in eval0; `lisp/boot/00-core.lisp` and `10-lambda-list.lisp`; `defmacro`, `defun`, `lambda` with full lambda lists via expansion, `let`-family, `cond`, `and`, `or`, `when`, `unless`, backquote, `defpackage`, `in-package`, `defvar`, `defparameter`; a minimal `deftest` in boot; a stage-0 REPL (`pocketvm0 -r`).
**Acceptance:** `tests/conformance/` (≥200 assertions) passes on stage 0, including: every special operator in 4.5, all lambda-list keywords, a 1,000,000-iteration tail-recursive loop in constant memory, closures and mutation, `block`/`return-from`, `tagbody`/`go`, `catch`/`throw`, `unwind-protect` ordering, special-variable rebinding and restoration through non-local exit, multiple values, nested backquote, macros with `gensym` and `&environment`, packages. The suite passes under GC stress.

### M3: Compiler, VM, fasl loader, self-hosting

**Deliverables:** `lisp/compiler/*` (6.1); `vm.c` with the dynamic-state stack, special bindings, multiple values, and the inline-op slow-path protocol; `loader.c`; the `.fasl` format and `docs/bytecode.md`; stage0 → stage1 → stage2 bootstrap scripts; `%signal-error` hook installation.
**Acceptance:**
1. The full conformance suite passes on **VM-compiled code** with identical results to stage 0.
2. `stage1/compiler.fasl` is byte-identical to `stage2/compiler.fasl`.
3. `(fib 30)` on the VM is ≥5x faster than on stage 0 (record both).
4. Deep non-tail recursion (1,000,000 frames) signals a catchable `pocket:stack-overflow`, never crashes.
5. Tail calls in `defun` bodies without dynamic-extent constructs run in constant stack (tested).
6. Malformed `.fasl` files are rejected cleanly (fuzzed).
7. Everything passes under GC stress and ASAN.

### M4: Common Lisp language layer

**Deliverables:** `setf` machinery with `defsetf`, `define-setf-expander`, `(setf name)` functions, `incf decf push pop pushnew rotatef shiftf psetf`; `defstruct`; types (`typep`, `deftype`, `check-type`, `typecase`); control macros (`do do* dolist dotimes case ecase prog destructuring-bind multiple-value-*` and the rest of 4.5's derived forms); **`loop`**; the **condition system and restarts** (4.12) wired to `%signal-error` so that kernel errors (`(car 5)`, undefined function, unbound variable, divide by zero, fixnum overflow, stack overflow) become standard conditions.
**Acceptance:**
1. Conformance tests for each of the above (≥250 new assertions), including CLHS example cases for `loop`, `setf`, `defstruct`, `handler-case`, `handler-bind`, `restart-case`, `define-condition`.
2. `handler-bind` handlers run **before** unwinding (test); restarts are visible to handlers (test).
3. `(car 5)` signals a `type-error` with correct `datum` and `expected-type`; calling an undefined function signals `undefined-function`.
4. `(setf (gethash ...))`, `(setf (getf ...))`, `(incf (aref ...))` evaluate subforms exactly once and in order.
5. The compiler and boot sources are recompiled with the new language layer and the fixed point still holds.

### M5: Common Lisp library I (data)

**Deliverables:** lists (full), sequences (full, with keyword arguments), strings and characters, numbers above the primitives (`floor round gcd lcm expt isqrt min max abs signum random`), arrays (`make-array`, adjustable vectors with fill pointers, `vector-push-extend`, rank ≥ 2), hash-table API (`maphash`, `remhash`, `clrhash`, `loop` support), symbols/`get`/`getf`, `sort` and `stable-sort`, `equalp`.
**Acceptance:** conformance tests (≥350 new assertions) pass on the VM; sequence functions agree across list, vector, and string arguments; `:key :test :test-not :start :end :from-end :count` behave per the CLHS; with `sbcl` present, `make diff-sbcl` shows no unexplained differences; `docs/conformance.md` is regenerated and passes `pocket conformance`; the suite passes under GC stress.

### M6: Common Lisp library II (I/O) and tooling

**Deliverables:** streams (string, console, tool-profile file streams); the **full printer**; the **full reader** (readtables); `format` (4.14) with the compile-time `format` compiler macro; `with-output-to-string`, `with-input-from-string`, `read-line` and friends; packages API completed (`defpackage` options, `do-symbols`); the `pocket.test` framework; `defsystem`-lite; the linker and tree-shaker; the `pocket` CLI (`build run test check conformance`, REPL basics); `docs/spec.md` complete.
**Acceptance:**
1. Reader parity: the C bootstrap reader and the Lisp reader agree on the M1 corpus; the Lisp reader passes tests for `set-macro-character`, `#+`/`#-`, `*read-eval*`, `readtable-case`, package-qualified names, error cases.
2. `read(print(x))` round-trips for a corpus of printable objects; `prin1` vs `princ` behavior per the CLHS; `format` directive tests (≥100 cases); a literal-control-string `format` compiles to direct code (verified by disassembly).
3. The compiler is switched to the Lisp reader and the bootstrap fixed point still holds.
4. `pocket test` runs all Lisp tests with a summary and correct exit code; a tree-shaking test (an unreferenced function is absent from the image); a `runtime` profile image runs without the compiler present; `pocket check` flags an unbound function, an excluded CL symbol (`defclass`), and a primitive arity error.

### M7: Headless platform, drawlist, frame loop, replay

**Deliverables:** `platform.h` ABI and `docs/platform.md`; `platform/common/raster.c`; `platform/headless`; `docs/drawlist.md`; Lisp drawlist emitter (`ui/draw.lisp`); `main.c` loop and `ui/runtime.lisp` (`frame`, `after`, `every`, event queue, input block); tape format and `pocket run --tape --hash`; built-in font atlas upload.
**Acceptance:**
1. `examples/rects` renders a deterministic image; its FNV hash matches the committed golden on Linux and macOS CI.
2. The text drawlist dump matches its golden.
3. **Frame determinism:** a test using `after` and an injected async event lands on the same frame in 100 runs, including `--chaos` runs.
4. A recorded tape replays to the same hash sequence.
5. A frame with no dirty state never calls `host-present` (counted).

### M8: Reactivity

**Deliverables:** `ui/signals.lisp` with the API in 8.2 and its tests.
**Acceptance:** all required-properties tests from 8.2 (diamond runs once, dynamic dependencies, owned and disposed effects, batching, cleanup order, no subscription leaks, deterministic order, `*observer*` restored on non-local exit); `let-signals` with `setf`/`incf` works; passes under GC stress; a micro-benchmark of 10,000 signal updates recorded.

### M9: UI tree, styles, layout, text, components (headless)

**Deliverables:** node arena; `view`, `text`, `show`, `each`, `mount` macros and runtime; compile-time class baking with `docs/style.md`; flexbox layout (8.5); text measure and wrap; dirty tracking; drawlist generation from the tree.
**Acceptance:**
1. `examples/counter` headless: golden hashes for the initial frame and after the tape's four A-presses; the green line appears at `n = 4`.
2. The ≥40-case layout test table passes.
3. `each` keyed-reorder test preserves node identity for retained keys.
4. Effects inside `show` and `each` are disposed on removal (leak counter returns to baseline).
5. A baked `:class` appears as a constant in the compiled image (no runtime class parsing for literal classes).
6. Idle frames present nothing.

### M10: Desktop backend, input, focus

**Deliverables:** `platform/sdl2`; keyboard and mouse mapping; spatial focus navigation; `:on-press`, `:on-focus`, `:on-blur`; `focus:` variants; pointer hit-testing; `pocket run --window`.
**Acceptance:** the counter app is interactive at a stable 60 fps; scripted focus tests (grid of buttons, D-pad traversal) pass headless; the SDL backend's framebuffer hash equals the headless hash for the same tape (parity); idle CPU use is near zero when nothing is dirty (the backend sleeps or waits on vsync).

### M11: WebAssembly playground

**Deliverables:** Emscripten build of the kernel with the compiler image; a static web page (editor, Run button, canvas, button controls) that compiles and runs PocketCL in the browser.
**Acceptance:** the counter example runs in the browser; native vs WASM parity: the same tape yields the same hash sequence (checked under Node); build size (wasm + image) is reported in `docs/perf.md`.

### M12: Performance gate

**Deliverables:** sampling profiler in Lisp over the VM hook; `bench/` suite covering every row in section 12; `pocket bench`; tuning of GC thresholds and frame-end collection; Lisp-side optimization (declarations, inline functions, compiler macros, allocation removal); optional VM-native `&optional`/`&key` and other Rule-of-Promotion primitives (each with its `reference-*` Lisp version and an equivalence test); `docs/perf.md`.
**Acceptance:** every row in section 12 is within its **gate** (and results are recorded against **targets**); every promoted primitive has an equivalence test; CI fails if a metric regresses by more than 20%.

### M13: Richer UI

**Deliverables:** `overflow-scroll` scroll views; `flex-wrap`; text input and an on-screen keyboard component; image asset pipeline (simple internal format; optional Lisp PNG decoder with inflate); font baking (optional vendored `stb_truetype` host tool); build-time animation (keyframes and springs) per 8.10; opacity; virtualized list.
**Acceptance:** each feature has golden tests and a replay tape; animations are frame-exact and deterministic under chaos mode; budgets in section 12 still hold.

### M14: Constrained-device port

**Deliverables:** one backend (PSP, 3DS, or ESP32-P4); runtime-only profile with no compiler and no eval0; image loaded from device storage; RGB565 rasterizer option; memory report.
**Acceptance:** the counter example boots and responds to input on hardware or a faithful emulator (PPSSPP, Azahar, or similar); total memory ≤ 8 MB (arena size recorded); measured frame time for a typical interaction is recorded and compared to the budget proxy; heap high-water mark reported; decisions on any needed promotions recorded.

### M15+: Backlog (post-v1, unordered)

- **PocketCLOS:** single-dispatch `defclass`/`defgeneric`/`defmethod`, then full CLOS if wanted.
- **Bignums and ratios in Lisp**, hooked through the inline-op slow-path protocol (5.5) so `+ - *` promote transparently; then `single-float` as a distinct type.
- Pretty printer; pathnames; more stream types; `progv`; `load-time-value`; wider Unicode case tables.
- Native modules (network, audio, storage) with opt-in linking.
- Time-travel devtools (tape plus state snapshots, inspector overlay emitted into the drawlist).
- Hygienic helper macros (or a `defmacro` linter for capture).
- Fibers and async/await on top of the dynamic-state stack.
- GPU backends; incremental or generational GC; damage-rect partial redraw.
- Narrow (8-bit) string representation for memory.
- Package manager; REPL-over-network for devices.

---

## 14. Risks and mitigations

| Risk | Why it matters | Mitigation |
|---|---|---|
| **CL's surface area is huge** | scope creep, large image sizes, slow progress | strict "subset, not dialect" rule; per-symbol `docs/conformance.md`; excluded symbols are not exported; tiers A/B/C; tree-shaking so unused library costs nothing; library is Lisp so it can be built incrementally against the conformance suite |
| **Silent divergence from ANSI** | users trust CL semantics | deviations fail loudly (principle 1); differential testing against SBCL; CLHS is the authority |
| **Lisp-written layout and drawlist generation is slow on a small CPU** | PocketJS puts these in Rust; here they run on a bytecode VM | struct-of-arrays node arena and fixnum-only layout; no allocation in hot paths; declarations and inlining; dirty-subtree relayout; idle frames free; budgets enforced from M12; Rule of Promotion as the escape hatch with reference implementations kept |
| **Lisp-2, special variables, multiple values, and conditions slow the VM** | each adds per-call or per-return cost | single-value fast path resets a count (one store); shallow binding for specials; function-cell calls with one instruction; handler clusters only cost when established; measured in section 12 |
| **GC pauses cause dropped frames** | stop-the-world collector | frame-end collection when above a soft threshold; small live heap by design; preallocated buffers; measured pause budget; incremental GC is backlog |
| **Two readers and two printers** (C bootstrap plus Lisp) | drift between them | the C versions are frozen to the minimal standard syntax; parity tests on a shared corpus; the compiler switches to the Lisp reader at M6 and the fixed point is re-verified |
| **Bootstrap fragility** | a compiler bug can poison later stages | stage 0 stays in tree and tested; byte-identical fixed-point check; deterministic fasl writer; conformance suite runs on both stages; boot files restricted to a tiny subset |
| **Non-hygienic macros capture names** | subtle bugs in UI macros | CL's own discipline: `gensym`, packages, Lisp-2; expansion tests |
| **31-bit fixnums on 32-bit targets** | integer overflow where CL programmers expect bignums | checked arithmetic signals `pocket:integer-overflow`; `fx+` wrapping ops for packing; layout math in pixels fits easily; bignum library on the backlog |
| **Golden tests flake across platforms** | determinism required | integer-only raster and layout; insertion-ordered tables and packages; content-based hashing; parity tests in M10 and M11 |
| **Text complexity** | Unicode is a bottomless pit | v1 is ASCII with a fallback glyph and Latin-1 case tables; wider scripts are explicit later work |
| **Upcase reader surprises newcomers** | `:class` is `:CLASS` in the image | documented; matches standard CL and SBCL; symbol names in the runtime image can be tree-shaken |

---

## 15. Decisions

### Locked (do not relitigate)

C99 kernel. PocketCL is a **Common Lisp subset**, Lisp-2, `NIL` false and empty list, standard `:upcase` reader. Conformance contract of 4.1 (excluded symbols not exported; deviations fail loudly). Packages with insertion-ordered iteration. Dynamic binding by shallow binding on a dynamic-state stack; one unwind mechanism for `block`/`catch`/`unwind-protect`/specials. Multiple values with a single-value fast path. Conditions and restarts implemented in Lisp, with kernel errors delivered through `%signal-error` as ordinary Lisp calls. No CLOS, bignums, ratios, `progv`, `load-time-value`, or pretty printer in v1. `defstruct` for structures; conditions are not CLOS classes. Single float format (IEEE double). Strings are mutable arrays of 32-bit characters at the Lisp level. Stage-0 tree-walking bootstrap evaluator plus stack bytecode VM; compiler-core lambda lists are required plus `&rest`, expanded in Lisp. Proper tail calls guaranteed in compiled code (except under dynamic-extent constructs). Build = compile-and-load into the compiler image and emit a fasl. Full reader and printer in Lisp; C reader and debug printer for bootstrap only. Mark-sweep non-moving GC at VM safe points, single arena. Frame-clocked deterministic runtime with tapes. Drawlist as the only render contract. Integer-only shared software rasterizer in C. Tailwind-compatible class subset baked by macros. Fine-grained reactivity with no VDOM, using `*observer*` and symbol macros. SDL2 for desktop. Headless-first testing. Source extension `.lisp`, image extension `.fasl`.

### Open (builder decides and records in `docs/decisions.md`)

Exact opcode encoding and operand widths; hash functions; drawlist record layout; free-list sweep details; system search path rules; error message wording; REPL features; whether to adopt the trivial inliner by default; whether signal writes compare with `eql` or `equal`; whether to add a narrow string representation; whether `the` is checked under `(safety 3)`; exact `format` directive set beyond 4.14; whether the tool profile exposes file streams through `open` or only `with-open-file`.

---

## 16. Definition of done for v1

All of M0 to M12 and M14 acceptance criteria pass in `make ci`; the counter example runs headless, on desktop, in the browser, and on one constrained device from the same source; the compiler is self-hosting with a byte-identical fixed point; `docs/conformance.md` accounts for every standard `COMMON-LISP` symbol and passes `pocket conformance`; the conformance suite passes on stage 0, the VM, and (when available) SBCL, with every difference either fixed or listed in 4.15; every budget in section 12 is met and documented in `docs/perf.md`; `docs/spec.md` describes the differences from the CLHS and the UI API accurately enough that a new contributor could write an app without reading the source.

---

## Appendix A: Common Lisp coverage by CLHS chapter

| Chapter | Status |
|---|---|
| 3 Evaluation and compilation | Supported: special operators in 4.5, `defmacro`, `macrolet`, `symbol-macrolet`, `define-symbol-macro`, `define-compiler-macro`, `macroexpand*`, `eval-when`, `declare`/`declaim`/`proclaim` (subset). `eval`, `compile`, `compile-file`, `load` in the dev profile only. **Excluded:** `load-time-value`, `progv`, `define-method-combination`. |
| 4 Types and classes | Supported: `typep`, `type-of`, `deftype`, `coerce` (subset), `subtypep` (basic). **Excluded:** `class-of`, `find-class`, CLOS class operations. |
| 5 Data and control flow | Supported: `defun`, `lambda`, `flet`, `labels`, `funcall`, `apply`, `setf` family, `multiple-value-*`, `values`, `block`/`return-from`, `tagbody`/`go`, `catch`/`throw`, `unwind-protect`, `prog` family, `case` family, `cond`, `and`/`or`/`when`/`unless`, `destructuring-bind`, `define-modify-macro`, `defsetf`, `define-setf-expander`, `get-setf-expansion`. **Excluded:** `defgeneric`, `defmethod`, `call-next-method`. |
| 6 Iteration | Supported: `do`, `do*`, `dolist`, `dotimes`, `loop` (4.14). |
| 7 Objects | **Excluded** (CLOS), except what chapters 8 and 9 provide. |
| 8 Structures | Supported: `defstruct` (4.13). |
| 9 Conditions | Supported (4.12). |
| 10 Symbols | Supported in full. |
| 11 Packages | Supported subset (4.8). |
| 12 Numbers | Fixnums and floats only (4.4); trig, `floor` family, `random` supported; ratios, bignums, complex excluded. |
| 13 Characters | Supported (code-point based; ASCII and Latin-1 case in v1). |
| 14 Conses | Supported in full. |
| 15 Arrays | Supported: vectors, strings, byte vectors, adjustable vectors, rank ≥ 2 in Lisp. **Excluded:** displaced arrays, most specialized element types. |
| 16 Strings | Supported in full (mutable character arrays). |
| 17 Sequences | Supported in full. |
| 18 Hash tables | Supported (`eq eql equal equalp`, insertion order). |
| 19 Filenames | **Excluded** (strings only). |
| 20 Files | Tool profile only: `open`, `close`, `with-open-file`, `probe-file`, text only. |
| 21 Streams | String streams, console streams, tool-profile file streams. **Excluded:** broadcast, concatenated, two-way, echo streams; binary `read-sequence`. |
| 22 Printer | Supported except pretty printing (`*print-pretty*` is always `NIL`; `pprint` excluded). |
| 23 Reader | Supported (4.3). |
| 24 System construction | `*features*`, `#+`/`#-`, `load`/`compile-file` in dev. **Excluded:** `require`, `provide`. Use `defsystem`-lite (7.3). |
| 25 Environment | Constants such as `lisp-implementation-type`; `time`, `describe`, `documentation`, `disassemble`, `get-internal-real-time` in the dev or tool profile only. **Excluded:** `ed`, `room`, `step`, `inspect`; `trace` is backlog. |
