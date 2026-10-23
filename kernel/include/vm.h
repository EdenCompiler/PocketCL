#ifndef POCKET_VM_H
#define POCKET_VM_H

#include "eval.h"

/* Compiled closures preserve PClosure's prefix: parameters=NIL, body=proto,
 * environment=flat capture vector. Macro flags may coexist with this bit. */
#define P_EVAL_COMPILED 4u
#define P_PROTO_VERIFIED 1u

typedef struct {
    uintptr_t header;
    Value name, code, constants;
    size_t required, locals, free_count, max_stack;
    unsigned target_bits, flags;
    bool rest;
} PProto;

typedef struct { uintptr_t header; Value value; } PBox;

/* These indices are also the argument order of SYS::%MAKE-PROTO and the
 * field order used by SYS::%PROTO-REF. They are independent of C struct layout. */
typedef enum {
    P_PROTO_NAME, P_PROTO_REQUIRED, P_PROTO_REST, P_PROTO_LOCALS,
    P_PROTO_FREE, P_PROTO_STACK, P_PROTO_TARGET_BITS,
    P_PROTO_CODE, P_PROTO_CONSTANTS, P_PROTO_FIELD_COUNT
} PProtoField;

/* One byte opcode, then fixed-width LE32 operands. Branch displacement is
 * signed and relative to the end of that instruction. CONST operands index
 * the current proto's constants vector; local/free operands index slots.
 *
 * CONST/GLOBAL/SET_GLOBAL/FUNCTION/CLOSURE/BIND_SPECIAL/PUSH_TAGBODY and
 * ADD/SUB/LT/NUM_EQ/CONS/CAR/CDR: u32 constant-pool index. Hot operations
 * name the global fallback function so redefinition still takes effect.
 * LOCAL/SET_LOCAL/FREE: u32 slot. CALL/TAIL_CALL/VALUES/POP_DYNAMIC: u32 count.
 * JUMP/JUMP_NIL/PUSH_CATCH/PUSH_BLOCK: signed relative i32 from instruction end.
 * GO: u32 label index, with a lexical tagbody token consumed from the stack.
 * Every other opcode has NO operand, including PUSH_UNWIND and RETURN_FROM.
 * PUSH_TAGBODY's constant is a vector of absolute byte offsets. */
typedef enum {
    P_OP_CONST = 1, P_OP_GLOBAL, P_OP_SET_GLOBAL, P_OP_FUNCTION,
    P_OP_LOCAL, P_OP_SET_LOCAL, P_OP_FREE, P_OP_POP, P_OP_DUP,
    P_OP_JUMP, P_OP_JUMP_NIL, P_OP_CALL, P_OP_TAIL_CALL, P_OP_RETURN,
    P_OP_CLOSURE, P_OP_BOX, P_OP_UNBOX, P_OP_SET_BOX, P_OP_VALUES,
    P_OP_MV_LIST, P_OP_MV_RESTORE, P_OP_BIND_SPECIAL,
    P_OP_PUSH_CATCH, P_OP_PUSH_BLOCK, P_OP_PUSH_TAGBODY, P_OP_PUSH_UNWIND,
    P_OP_POP_DYNAMIC, P_OP_THROW, P_OP_RETURN_FROM, P_OP_GO,
    P_OP_ADD, P_OP_SUB, P_OP_LT, P_OP_NUM_EQ, P_OP_CONS, P_OP_CAR, P_OP_CDR
} POpcode;

typedef struct {
    size_t value_capacity, frame_capacity, dynamic_capacity;
    size_t emergency_values, emergency_frames, emergency_bytes;
} PVMConfig;

/* Scalar implementation details live in the arena owner. Values within it
 * have explicit root ranges; an untraced BYTES object alone is not a root for
 * its contents. Keep this host handle alive until p_vm_destroy. */
typedef struct PVM {
    PState *state;
    Value owner;
    void *implementation;
    size_t saved_root_count;
    size_t value_root_index;
    size_t error_root_base;
    size_t error_hook_frame;
    Value emergency_heap_reserve;
    Value error_values[2];
    PError pending_error;
    Value unwind_values[P_MULTIPLE_VALUES_LIMIT + 2u];
    size_t unwind_value_count;
    PVMConfig config;
    bool signaling_error;
    bool emergency_mode;
    bool error_hook_completed;
    bool initialized;
} PVM;

void p_vm_default_config(PVMConfig *config);
bool p_vm_init(PVM *vm, PState *state, const PVMConfig *config);
void p_vm_destroy(PVM *vm);
Value p_vm_call(PVM *vm, Value function, size_t argc, const Value *argv);

/* Constructors, verification, and primitive entries do not collect. Their
 * caller must preflight allocation while inputs are rooted. The verifier's
 * scratch buffer contains only scalar work arrays, and can be reused across
 * all protos in a loaded image. No host malloc is needed. */
Value p_vm_make_proto(Pocket *p, const Value fields[P_PROTO_FIELD_COUNT]);
Value p_vm_make_closure(Pocket *p, Value proto, Value captures);
bool p_vm_verify_workspace(size_t code_length, size_t *bytes);
bool p_vm_verify(Pocket *p, Value proto, void *workspace, size_t workspace_size);
bool p_install_vm_primitives(PState *state);

#endif
