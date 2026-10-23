# Bytecode and image format

M3 implementation contract, validated by the local acceptance gates. Opcode
numbers live in `kernel/include/vm.h`. The loader is `kernel/src/loader.c` and
the writer is ordinary Lisp in `lisp/compiler/50-fasl.lisp`. A changed wire
layout requires a new format version.

## Values and calls

A prototype owns a name, bytecode, constant vector, required-argument count,
rest-argument flag, local/free slot counts, maximum operand depth, and target
fixnum width. A compiled closure pairs a prototype with a flat capture vector.
Assigned and captured variables use explicit boxes; immutable captures are copied.

Calls place required arguments in the first local slots, followed by the rest
list when present. Remaining locals begin unbound. Operands grow above locals.
The operand stack carries one primary result; a separate buffer carries the
exact multiple-value count, including zero. Value-producing instructions set
one value; stack plumbing and branches preserve the buffer. `VALUES 0` pushes
primary NIL while retaining zero values. Function return preserves all values.

The VM owns explicit frames and dynamic entries in its startup arena. Tail
calls may reuse a frame only when no dynamic entries remain above its entry
depth. The compiler omits implicit blocks proven unused after expansion.
Native APPLY and FUNCALL descriptors redirect ordinary engine dispatch; their
primitive entries never call Lisp. Hot arithmetic/list opcodes name a fallback
global function and must respect redefinition of that function.

## Encoding and instructions

Each instruction begins with one byte. Every operand occupies four bytes in
little-endian order. Relative jumps are signed two's-complement displacements
from the end of the instruction, measured in bytes. Other operands are unsigned.
A constant index always addresses the current prototype's pool.

| Opcode | Name | Operand | Stack action |
| --- | --- | --- | --- |
| 1 | CONST | constant | Push literal |
| 2 | GLOBAL | symbol constant | Push symbol value |
| 3 | SET_GLOBAL | symbol constant | Assign top; retain value |
| 4 | FUNCTION | symbol constant | Push function cell |
| 5 | LOCAL | local slot | Push local |
| 6 | SET_LOCAL | local slot | Assign top; retain value |
| 7 | FREE | capture slot | Push capture |
| 8 | POP | — | Drop primary stack item |
| 9 | DUP | — | Duplicate top |
| 10 | JUMP | relative target | Branch |
| 11 | JUMP_NIL | relative target | Pop test; branch on NIL |
| 12 | CALL | argument count | Function, arguments → primary result |
| 13 | TAIL_CALL | argument count | Reuse eligible frame |
| 14 | RETURN | — | Return primary and multiple values |
| 15 | CLOSURE | prototype constant | Captures → closure |
| 16 | BOX | — | Value → box |
| 17 | UNBOX | — | Box → value |
| 18 | SET_BOX | — | Box, value → value |
| 19 | VALUES | value count | Values → primary with exact value count |
| 20 | MV_LIST | — | Primary → proper list of current values |
| 21 | MV_RESTORE | — | Proper list → primary and saved values |
| 22 | BIND_SPECIAL | symbol constant | Pop value; save dynamic binding |
| 23 | PUSH_CATCH | relative exit target | Pop tag; save dynamic catch |
| 24 | PUSH_BLOCK | relative exit target | Push fresh lexical block token |
| 25 | PUSH_TAGBODY | label-table constant | Push fresh lexical tagbody token |
| 26 | PUSH_UNWIND | — | Pop cleanup closure; save dynamic cleanup |
| 27 | POP_DYNAMIC | entry count | Restore entries and run cleanups |
| 28 | THROW | — | Tag, primary → dynamic transfer |
| 29 | RETURN_FROM | — | Block token, primary → lexical transfer |
| 30 | GO | label index | Pop lexical tagbody token; transfer |
| 31 | ADD | fallback symbol constant | Two operands → sum |
| 32 | SUB | fallback symbol constant | Two operands → difference |
| 33 | LT | fallback symbol constant | Two operands → comparison |
| 34 | NUM_EQ | fallback symbol constant | Two operands → comparison |
| 35 | CONS | fallback symbol constant | Two operands → cons |
| 36 | CAR | fallback symbol constant | One operand → car |
| 37 | CDR | fallback symbol constant | One operand → cdr |

Catch tags compare by EQ. Block/tagbody tokens identify lexical activations,
including those captured by closures. Label tables contain absolute instruction
offsets. GO has no fallthrough edge and restores the operand height saved
before PUSH_TAGBODY pushed its token. The verifier checks every label at that
height, including labels reachable only by GO. THROW and RETURN_FROM transfer
all multiple values. Normal and exceptional
unwind preserve pending values while cleanup closures execute. A cleanup can
supersede a pending transfer.

For example, a prototype returning pool element 0 contains six bytes:

```
01 00 00 00 00   CONST 0
0e               RETURN
```

## PFAS version 1

PFAS contains IDs and bytes, with no host pointers, C struct padding or native
function addresses. Integers have fixed little-endian widths. Binary64 floats
use their IEEE representation. A loader rejects an unsupported float host.

| Header offset | Width | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `PFAS` |
| 4 | 2 | Version, currently 1 |
| 6 | 2 | Flags: bit 0 compiler image, bit 1 debug, bit 8 31-bit fixnums |
| 8 | 4 | Total image byte length |
| 12 | 4 | Section count, exactly 4 |

Without bit 8 the fixnum width is 62, including its sign bit. Unknown flag bits
and target-width mismatches are errors. Integer constants must fit the declared
signed target width.

Four sections appear in order. Each has a u32 ID, a u32 payload byte length,
then a payload beginning with a u32 record count. IDs are zero-based within
their tables. Every section must end exactly at its declared end.

1. **Symbols (ID 1).** Each record has u32 package-name UTF-8 byte length,
   package bytes, u32 symbol-name byte length, and name bytes. Package length
   `0xffffffff` means uninterned and has no package bytes. Distinct uninterned
   IDs retain distinct identity even with equal names. Interned symbols use
   their home package, including local shadowing of inherited symbols. Names
   cannot contain NUL in this kernel version.
2. **Constants (ID 2).** Each record has a one-byte tag, u32 payload length,
   and payload described below. Forward references and cycles are allowed.
3. **Prototypes (ID 3).** Each record has nine u32 fields: name constant ID,
   required count, rest flag (0/1), local count, free count, maximum operand
   depth, target fixnum width, code constant ID, and pool constant ID. Names
   are symbols; code is bytes and the pool is a simple vector.
4. **Entry (ID 4).** Exactly one record containing a prototype ID. The entry
   has zero required arguments, no rest argument and no captures.

| Constant tag | Kind | Payload |
| --- | --- | --- |
| 1 | Fixnum | Signed i64 |
| 2 | Character | u32 Unicode scalar |
| 3 | Float | Eight binary64 bytes |
| 4 | String | UTF-8 bytes; NUL is allowed |
| 5 | Symbol | u32 symbol ID |
| 6 | Cons | u32 car constant ID, u32 cdr constant ID |
| 7 | Simple vector | u32 length followed by constant IDs |
| 8 | Bytes | Raw bytes |
| 9 | Prototype | u32 prototype ID |

The Lisp writer assigns IDs using a first-visit work queue. Identity hashes
assist lookup; hash-bucket order and addresses never determine output. The
script entry calls retained source initializers in order and returns NIL.
The host does not infer compiler policy from top-level forms.

## Loading and validation

The host roots input, calls `p_fasl_required_bytes`, then reserves the reported
arena capacity at a GC safe point. `p_fasl_load` performs no collection and
executes no Lisp. It builds skeletons before filling references, allowing cycles,
and returns a verified entry closure for an ordinary VM call. Temporary value
arrays are explicitly rooted even when backed by a byte object.

The parser checks extents, arithmetic overflow, exact record counts, UTF-8,
scalar ranges, reference bounds/kinds, prototype metadata and entry shape. The
shared verifier checks opcode extents, instruction boundaries, branch destinations,
indices and stack requirements before execution. Package identity changes occur
after structural and instruction validation. Allocation exhaustion reports a
structured error. Mutation fuzzing exercises rejection without running arbitrary
accepted bytecode.

Values and lexical control tokens still require runtime type checks. In
particular, a GO can capture a tagbody in an enclosing function, so its label
index is checked against that activation's table when the instruction executes.
Static verification of the instruction stream does not imply static typing of
all operand values.
