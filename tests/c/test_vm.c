#include "vm.h"
#include "exec.h"
#include "test.h"

#include <string.h>

static void put_u32(uint8_t *code, size_t *at, uint32_t value)
{
    code[(*at)++] = (uint8_t)value;
    code[(*at)++] = (uint8_t)(value >> 8);
    code[(*at)++] = (uint8_t)(value >> 16);
    code[(*at)++] = (uint8_t)(value >> 24);
}

static bool interpreted_arguments_rooted;

static Value fallback_root_probe(PState *state, Value function, size_t argc,
                                 const Value *argv)
{
    uintptr_t args_begin = (uintptr_t)argv;
    uintptr_t args_end = args_begin + argc * sizeof(Value);
    (void)function;
    interpreted_arguments_rooted = false;
    for (size_t i = 0u; i < state->p->root_count; ++i) {
        uintptr_t root_begin = (uintptr_t)state->p->roots[i].values;
        uintptr_t root_end = root_begin + state->p->roots[i].count * sizeof(Value);
        if (args_begin >= root_begin && args_end <= root_end) {
            interpreted_arguments_rooted = true;
            break;
        }
    }
    state->mv[0] = p_fixnum(42);
    state->mv_count = 1u;
    return p_fixnum(42);
}

void test_vm(void)
{
    Pocket p;
    PState state;
    PVM vm = { 0 };
    Value roots[56] = { 0u };
    size_t saved;
    ASSERT(p_init(&p, 4u * 1024u * 1024u));
    ASSERT(p_state_init(&state, &p));
    ASSERT(p_install_primitives(&state));
    ASSERT(p_install_vm_primitives(&state));
    ASSERT(p_vm_init(&vm, &state, NULL));
    saved = p_root_save(&p);
    ASSERT(p_roots(&p, roots, 56u));

    /* A tiny verified proto adds two constants and returns the result. */
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 32u +
                     offsetof(PVector, data) + 4u * sizeof(Value) +
                     sizeof(PProto) + 1024u));
    roots[0] = p_bytes(&p, 32u);
    roots[1] = p_vector(&p, 3u, p.nil);
    ASSERT(roots[0] != 0u && roots[1] != 0u);
    PVector *constants = p_vector_ptr(roots[1]);
    constants->data[0] = p_fixnum(19);
    constants->data[1] = p_fixnum(23);
    roots[2] = p_intern(&p, p.cl, "+", NULL);
    ASSERT(roots[2] != 0u);
    constants->data[2] = roots[2];
    uint8_t *code = ((PBytes *)roots[0])->data;
    size_t at = 0u;
    code[at++] = P_OP_CONST; put_u32(code, &at, 0u);
    code[at++] = P_OP_CONST; put_u32(code, &at, 1u);
    code[at++] = P_OP_ADD; put_u32(code, &at, 2u);
    code[at++] = P_OP_RETURN;
    ((PBytes *)roots[0])->length = at;
    Value fields[P_PROTO_FIELD_COUNT] = {
        p.nil, p_fixnum(0), p.nil, p_fixnum(0), p_fixnum(0),
        p_fixnum(2), p_fixnum((int64_t)p_fixnum_bits()), roots[0], roots[1]
    };
    roots[3] = p_vm_make_proto(&p, fields);
    ASSERT(roots[3] != 0u);
    size_t workspace_size;
    ASSERT(p_vm_verify_workspace(at, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[4] = p_bytes(&p, workspace_size);
    ASSERT(roots[4] != 0u);
    ASSERT(p_vm_verify(&p, roots[3], ((PBytes *)roots[4])->data,
                       workspace_size));
    ASSERT((((PProto *)roots[3])->flags & P_PROTO_VERIFIED) != 0u);
    ASSERT(p_reserve(&p, offsetof(PVector, data) + sizeof(PClosure)));
    roots[3] = p_vm_make_closure(&p, roots[3], p_vector(&p, 0u, p.nil));
    ASSERT(roots[3] != 0u);
    ASSERT(p_vm_call(&vm, roots[3], 0u, NULL) == p_fixnum(42));
    ASSERT(state.mv_count == 1u && state.mv[0] == p_fixnum(42));

    /* A THROW carries the producer's complete multiple-value packet through
     * the matching EQ catch and returns its primary on the operand stack. */
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 32u +
                     offsetof(PVector, data) + 3u * sizeof(Value) +
                     sizeof(PProto) + offsetof(PBytes, data) + 512u +
                     offsetof(PVector, data) + sizeof(PClosure)));
    roots[6] = p_bytes(&p, 32u);
    roots[7] = p_vector(&p, 3u, p.nil);
    ASSERT(roots[6] != 0u && roots[7] != 0u);
    roots[8] = p_intern(&p, p.cl, "VM-CATCH-TAG", NULL);
    ASSERT(roots[8] != 0u);
    p_vector_ptr(roots[7])->data[0] = roots[8];
    p_vector_ptr(roots[7])->data[1] = p_fixnum(19);
    p_vector_ptr(roots[7])->data[2] = p_fixnum(23);
    uint8_t *catch_bytes = ((PBytes *)roots[6])->data;
    at = 0u;
    catch_bytes[at++] = P_OP_CONST; put_u32(catch_bytes, &at, 0u);
    catch_bytes[at++] = P_OP_PUSH_CATCH; put_u32(catch_bytes, &at, 21u);
    catch_bytes[at++] = P_OP_CONST; put_u32(catch_bytes, &at, 0u);
    catch_bytes[at++] = P_OP_CONST; put_u32(catch_bytes, &at, 1u);
    catch_bytes[at++] = P_OP_CONST; put_u32(catch_bytes, &at, 2u);
    catch_bytes[at++] = P_OP_VALUES; put_u32(catch_bytes, &at, 2u);
    catch_bytes[at++] = P_OP_THROW;
    catch_bytes[at++] = P_OP_RETURN;
    ASSERT(at == 32u);
    ((PBytes *)roots[6])->length = at;
    fields[P_PROTO_CODE] = roots[6];
    fields[P_PROTO_CONSTANTS] = roots[7];
    fields[P_PROTO_STACK] = p_fixnum(3);
    roots[9] = p_vm_make_proto(&p, fields);
    ASSERT(roots[9] != 0u);
    ASSERT(p_vm_verify_workspace(at, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[10] = p_bytes(&p, workspace_size);
    ASSERT(roots[10] != 0u);
    ASSERT(p_vm_verify(&p, roots[9], ((PBytes *)roots[10])->data,
                       workspace_size));
    ASSERT(p_reserve(&p, offsetof(PVector, data) + sizeof(PClosure)));
    roots[11] = p_vector(&p, 0u, p.nil);
    ASSERT(roots[11] != 0u);
    roots[12] = p_vm_make_closure(&p, roots[9], roots[11]);
    ASSERT(roots[12] != 0u);
    Value catch_result = p_vm_call(&vm, roots[12], 0u, NULL);
    ASSERT(catch_result == p_fixnum(19));
    ASSERT(state.mv_count == 2u && state.mv[0] == p_fixnum(19) &&
           state.mv[1] == p_fixnum(23));

    /* An unhandled VM error restores dynamic special bindings after the hook
     * has had its chance to inspect the live environment. */
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 21u +
                     offsetof(PVector, data) + 3u * sizeof(Value) +
                     sizeof(PProto) + offsetof(PBytes, data) + 512u +
                     offsetof(PVector, data) + sizeof(PClosure)));
    roots[39] = p_intern(&p, p.cl, "VM-SPECIAL", NULL);
    roots[40] = p_vector(&p, 3u, p.nil);
    roots[41] = p_bytes(&p, 21u);
    ASSERT(roots[39] && roots[40] && roots[41]);
    p_symbol_ptr(roots[39])->flags |= P_SPECIAL;
    p_symbol_ptr(roots[39])->value = p_fixnum(99);
    p_vector_ptr(roots[40])->data[0] = roots[39];
    p_vector_ptr(roots[40])->data[1] = p_fixnum(17);
    p_vector_ptr(roots[40])->data[2] = p_fixnum(42);
    uint8_t *error_code = ((PBytes *)roots[41])->data;
    at = 0u;
    error_code[at++] = P_OP_CONST; put_u32(error_code, &at, 1u);
    error_code[at++] = P_OP_BIND_SPECIAL; put_u32(error_code, &at, 0u);
    error_code[at++] = P_OP_CONST; put_u32(error_code, &at, 2u);
    error_code[at++] = P_OP_CALL; put_u32(error_code, &at, 0u);
    error_code[at++] = P_OP_RETURN;
    ASSERT(at == 21u);
    ((PBytes *)roots[41])->length = at;
    fields[P_PROTO_CODE] = roots[41];
    fields[P_PROTO_CONSTANTS] = roots[40];
    fields[P_PROTO_REQUIRED] = p_fixnum(0);
    fields[P_PROTO_LOCALS] = p_fixnum(0);
    fields[P_PROTO_STACK] = p_fixnum(1);
    roots[42] = p_vm_make_proto(&p, fields);
    ASSERT(roots[42] && p_vm_verify_workspace(at, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[43] = p_bytes(&p, workspace_size);
    ASSERT(p_vm_verify(&p, roots[42], ((PBytes *)roots[43])->data,
                       workspace_size));
    roots[44] = p_vector(&p, 0u, p.nil);
    ASSERT(p_reserve(&p, offsetof(PVector, data) + sizeof(PClosure)));
    roots[45] = p_vm_make_closure(&p, roots[42], roots[44]);
    ASSERT(roots[45]);
    p_clear_error(&p);
    ASSERT(p_vm_call(&vm, roots[45], 0u, NULL) == 0u);
    ASSERT(p.error.code == P_ERR_TYPE && p_symbol_ptr(roots[39])->value == p_fixnum(99));
    p_clear_error(&p);

    /* Stack errors enter the ordinary VM error hook while the catch is still
     * active; the hook can THROW into it and replace the failing operation. */
    ASSERT(p_reserve(&p, 2u * (offsetof(PBytes, data) + 32u) +
                     2u * (offsetof(PVector, data) + 2u * sizeof(Value)) +
                     2u * sizeof(PProto) + 2u * (offsetof(PBytes, data) + 512u) +
                     2u * (offsetof(PVector, data) + sizeof(PClosure))));
    roots[13] = p_vector(&p, 2u, p.nil);
    roots[14] = p_bytes(&p, 22u);
    roots[15] = p_vector(&p, 2u, p.nil);
    roots[16] = p_bytes(&p, 11u);
    ASSERT(roots[13] && roots[14] && roots[15] && roots[16]);
    p_vector_ptr(roots[13])->data[0] = roots[8];
    p_vector_ptr(roots[13])->data[1] = p_fixnum(42);
    p_vector_ptr(roots[15])->data[0] = roots[8];
    p_vector_ptr(roots[15])->data[1] = p_fixnum(77);
    uint8_t *parent_code = ((PBytes *)roots[14])->data;
    at = 0u;
    parent_code[at++] = P_OP_CONST; put_u32(parent_code, &at, 0u);
    parent_code[at++] = P_OP_PUSH_CATCH; put_u32(parent_code, &at, 11u);
    parent_code[at++] = P_OP_CONST; put_u32(parent_code, &at, 1u);
    parent_code[at++] = P_OP_CALL; put_u32(parent_code, &at, 0u);
    parent_code[at++] = P_OP_RETURN;
    parent_code[at++] = P_OP_RETURN;
    ASSERT(at == 22u);
    ((PBytes *)roots[14])->length = at;
    uint8_t *hook_code = ((PBytes *)roots[16])->data;
    at = 0u;
    hook_code[at++] = P_OP_CONST; put_u32(hook_code, &at, 0u);
    hook_code[at++] = P_OP_CONST; put_u32(hook_code, &at, 1u);
    hook_code[at++] = P_OP_THROW;
    ASSERT(at == 11u);
    ((PBytes *)roots[16])->length = at;

    fields[P_PROTO_CODE] = roots[14];
    fields[P_PROTO_CONSTANTS] = roots[13];
    fields[P_PROTO_REQUIRED] = p_fixnum(0);
    fields[P_PROTO_LOCALS] = p_fixnum(0);
    fields[P_PROTO_STACK] = p_fixnum(1);
    roots[17] = p_vm_make_proto(&p, fields);
    fields[P_PROTO_CODE] = roots[16];
    fields[P_PROTO_CONSTANTS] = roots[15];
    fields[P_PROTO_REQUIRED] = p_fixnum(3);
    fields[P_PROTO_LOCALS] = p_fixnum(3);
    fields[P_PROTO_STACK] = p_fixnum(2);
    roots[18] = p_vm_make_proto(&p, fields);
    ASSERT(roots[17] && roots[18]);
    ASSERT(p_vm_verify_workspace(22u, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 2u * workspace_size));
    roots[19] = p_bytes(&p, workspace_size);
    ASSERT(p_vm_verify(&p, roots[17], ((PBytes *)roots[19])->data,
                       workspace_size));
    roots[20] = p_bytes(&p, workspace_size);
    ASSERT(p_vm_verify_workspace(11u, &at));
    /* Reuse roots[20] at the smaller exact size for the hook proto. */
    roots[20] = p_bytes(&p, at);
    ASSERT(p_vm_verify(&p, roots[18], ((PBytes *)roots[20])->data, at));
    ASSERT(p_reserve(&p, offsetof(PVector, data) + sizeof(PClosure)));
    roots[21] = p_vector(&p, 0u, p.nil);
    roots[22] = p_vm_make_closure(&p, roots[17], roots[21]);
    roots[23] = p_vm_make_closure(&p, roots[18], roots[21]);
    ASSERT(roots[22] && roots[23]);
    p.error_hook = roots[23];
    ASSERT(p_vm_call(&vm, roots[22], 0u, NULL) == p_fixnum(77));
    ASSERT(p.error.code == P_OK && state.mv_count == 1u &&
           state.mv[0] == p_fixnum(77));
    p.error_hook = p.nil;

    /* APPLY keeps each fixed prefix argument while flattening the final list. */
    roots[24] = p_intern(&p, p.cl, "APPLY", NULL);
    roots[25] = p_intern(&p, p.cl, "+", NULL);
    ASSERT(roots[24] && roots[25]);
    ASSERT(p_reserve(&p, 2u * sizeof(PCons)));
    roots[26] = p_cons(&p, p_fixnum(7), p.nil);
    roots[27] = p_cons(&p, p_fixnum(6), roots[26]);
    ASSERT(roots[26] && roots[27]);
    Value apply_args[4] = { roots[25], p_fixnum(4), p_fixnum(5), roots[27] };
    ASSERT(p_vm_call(&vm, roots[24], 4u, apply_args) == p_fixnum(22));
    Value apply_empty[2] = { roots[25], p.nil };
    ASSERT(p_vm_call(&vm, roots[24], 2u, apply_empty) == p_fixnum(0));
    Value apply_list_only[2] = { roots[25], roots[27] };
    ASSERT(p_vm_call(&vm, roots[24], 2u, apply_list_only) == p_fixnum(13));

    /* POP-DYNAMIC runs cleanup through the ordinary call path while restoring
     * the body's complete multiple-value packet. */
    ASSERT(p_reserve(&p, 2u * (offsetof(PBytes, data) + 32u) +
                     2u * (offsetof(PVector, data) + 3u * sizeof(Value)) +
                     2u * sizeof(PProto) + 2u * (offsetof(PBytes, data) + 512u) +
                     offsetof(PVector, data) + sizeof(PClosure)));
    roots[28] = p_bytes(&p, 6u);
    roots[29] = p_vector(&p, 1u, p.nil);
    roots[30] = p_bytes(&p, 27u);
    roots[31] = p_vector(&p, 3u, p.nil);
    ASSERT(roots[28] && roots[29] && roots[30] && roots[31]);
    p_vector_ptr(roots[29])->data[0] = p_fixnum(77);
    uint8_t *cleanup_code = ((PBytes *)roots[28])->data;
    at = 0u;
    cleanup_code[at++] = P_OP_CONST; put_u32(cleanup_code, &at, 0u);
    cleanup_code[at++] = P_OP_RETURN;
    ASSERT(at == 6u);
    ((PBytes *)roots[28])->length = at;
    fields[P_PROTO_CODE] = roots[28];
    fields[P_PROTO_CONSTANTS] = roots[29];
    fields[P_PROTO_REQUIRED] = p_fixnum(0);
    fields[P_PROTO_LOCALS] = p_fixnum(0);
    fields[P_PROTO_STACK] = p_fixnum(1);
    roots[32] = p_vm_make_proto(&p, fields);
    ASSERT(roots[32] && p_vm_verify_workspace(6u, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[33] = p_bytes(&p, workspace_size);
    ASSERT(p_vm_verify(&p, roots[32], ((PBytes *)roots[33])->data,
                       workspace_size));
    roots[34] = p_vector(&p, 0u, p.nil);
    ASSERT(p_reserve(&p, offsetof(PVector, data) + sizeof(PClosure)));
    roots[35] = p_vm_make_closure(&p, roots[32], roots[34]);
    ASSERT(roots[35]);
    p_vector_ptr(roots[31])->data[0] = roots[35];
    p_vector_ptr(roots[31])->data[1] = p_fixnum(19);
    p_vector_ptr(roots[31])->data[2] = p_fixnum(23);
    uint8_t *unwind_code = ((PBytes *)roots[30])->data;
    at = 0u;
    unwind_code[at++] = P_OP_CONST; put_u32(unwind_code, &at, 0u);
    unwind_code[at++] = P_OP_PUSH_UNWIND;
    unwind_code[at++] = P_OP_CONST; put_u32(unwind_code, &at, 1u);
    unwind_code[at++] = P_OP_CONST; put_u32(unwind_code, &at, 2u);
    unwind_code[at++] = P_OP_VALUES; put_u32(unwind_code, &at, 2u);
    unwind_code[at++] = P_OP_POP_DYNAMIC; put_u32(unwind_code, &at, 1u);
    unwind_code[at++] = P_OP_RETURN;
    ASSERT(at == 27u);
    ((PBytes *)roots[30])->length = at;
    fields[P_PROTO_CODE] = roots[30];
    fields[P_PROTO_CONSTANTS] = roots[31];
    fields[P_PROTO_STACK] = p_fixnum(2);
    roots[36] = p_vm_make_proto(&p, fields);
    ASSERT(roots[36] && p_vm_verify_workspace(27u, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[37] = p_bytes(&p, workspace_size);
    ASSERT(p_vm_verify(&p, roots[36], ((PBytes *)roots[37])->data,
                       workspace_size));
    roots[38] = p_vm_make_closure(&p, roots[36], roots[34]);
    ASSERT(roots[38]);
    ASSERT(p_vm_call(&vm, roots[38], 0u, NULL) == p_fixnum(19));
    ASSERT(state.mv_count == 2u && state.mv[0] == p_fixnum(19) &&
           state.mv[1] == p_fixnum(23));

    /* Cleanup also preserves the distinct zero-values state. */
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 17u + sizeof(PProto) +
                     offsetof(PBytes, data) + 512u +
                     offsetof(PVector, data) + sizeof(PClosure)));
    roots[46] = p_bytes(&p, 17u);
    ASSERT(roots[46]);
    uint8_t *zero_mv_code = ((PBytes *)roots[46])->data;
    at = 0u;
    zero_mv_code[at++] = P_OP_CONST; put_u32(zero_mv_code, &at, 0u);
    zero_mv_code[at++] = P_OP_PUSH_UNWIND;
    zero_mv_code[at++] = P_OP_VALUES; put_u32(zero_mv_code, &at, 0u);
    zero_mv_code[at++] = P_OP_POP_DYNAMIC; put_u32(zero_mv_code, &at, 1u);
    zero_mv_code[at++] = P_OP_RETURN;
    ASSERT(at == 17u);
    ((PBytes *)roots[46])->length = at;
    fields[P_PROTO_CODE] = roots[46];
    fields[P_PROTO_CONSTANTS] = roots[31];
    fields[P_PROTO_STACK] = p_fixnum(1);
    roots[47] = p_vm_make_proto(&p, fields);
    ASSERT(roots[47] && p_vm_verify_workspace(at, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[48] = p_bytes(&p, workspace_size);
    ASSERT(p_vm_verify(&p, roots[47], ((PBytes *)roots[48])->data,
                       workspace_size));
    roots[49] = p_vm_make_closure(&p, roots[47], roots[34]);
    ASSERT(roots[49]);
    ASSERT(p_vm_call(&vm, roots[49], 0u, NULL) == p.nil);
    ASSERT(state.mv_count == 0u);

    /* GO consumes its token and restores the tagbody's operand baseline. */
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 26u +
                     offsetof(PVector, data) + 4u * sizeof(Value) +
                     offsetof(PVector, data) + sizeof(Value) + sizeof(PProto) +
                     offsetof(PBytes, data) + 512u +
                     offsetof(PVector, data) + sizeof(PClosure)));
    roots[46] = p_bytes(&p, 26u);
    roots[48] = p_vector(&p, 1u, p.nil);
    roots[50] = p_vector(&p, 4u, p.nil);
    ASSERT(roots[46] && roots[48] && roots[50]);
    roots[49] = p_intern(&p, p.cl, "+", NULL);
    ASSERT(roots[49]);
    p_vector_ptr(roots[48])->data[0] = p_fixnum(20);
    p_vector_ptr(roots[50])->data[0] = p_fixnum(19);
    p_vector_ptr(roots[50])->data[1] = p_fixnum(23);
    p_vector_ptr(roots[50])->data[2] = roots[49];
    p_vector_ptr(roots[50])->data[3] = roots[48];
    uint8_t *go_code = ((PBytes *)roots[46])->data;
    at = 0u;
    go_code[at++] = P_OP_CONST; put_u32(go_code, &at, 0u);
    go_code[at++] = P_OP_CONST; put_u32(go_code, &at, 1u);
    go_code[at++] = P_OP_PUSH_TAGBODY; put_u32(go_code, &at, 3u);
    go_code[at++] = P_OP_GO; put_u32(go_code, &at, 0u);
    go_code[at++] = P_OP_ADD; put_u32(go_code, &at, 2u);
    go_code[at++] = P_OP_RETURN;
    ASSERT(at == 26u);
    ((PBytes *)roots[46])->length = at;
    fields[P_PROTO_CODE] = roots[46];
    fields[P_PROTO_CONSTANTS] = roots[50];
    fields[P_PROTO_STACK] = p_fixnum(3);
    roots[51] = p_vm_make_proto(&p, fields);
    ASSERT(roots[51] && p_vm_verify_workspace(at, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[52] = p_bytes(&p, workspace_size);
    ASSERT(p_vm_verify(&p, roots[51], ((PBytes *)roots[52])->data,
                       workspace_size));
    roots[53] = p_vm_make_closure(&p, roots[51], roots[34]);
    ASSERT(roots[53]);
    ASSERT(p_vm_call(&vm, roots[53], 0u, NULL) == p_fixnum(42));

    /* The label is reachable only through GO.  POP immediately after GO
     * would underflow if the verifier incorrectly followed GO's fallthrough. */
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 17u +
                     offsetof(PVector, data) + 2u * sizeof(Value) +
                     offsetof(PVector, data) + sizeof(Value) + sizeof(PProto) +
                     offsetof(PBytes, data) + 512u +
                     offsetof(PVector, data) + sizeof(PClosure)));
    roots[46] = p_bytes(&p, 17u);
    roots[48] = p_vector(&p, 1u, p.nil);
    roots[50] = p_vector(&p, 2u, p.nil);
    ASSERT(roots[46] && roots[48] && roots[50]);
    p_vector_ptr(roots[48])->data[0] = p_fixnum(11);
    p_vector_ptr(roots[50])->data[0] = roots[48];
    p_vector_ptr(roots[50])->data[1] = p_fixnum(77);
    uint8_t *label_code = ((PBytes *)roots[46])->data;
    at = 0u;
    label_code[at++] = P_OP_PUSH_TAGBODY; put_u32(label_code, &at, 0u);
    label_code[at++] = P_OP_GO; put_u32(label_code, &at, 0u);
    label_code[at++] = P_OP_POP;
    label_code[at++] = P_OP_CONST; put_u32(label_code, &at, 1u);
    label_code[at++] = P_OP_RETURN;
    ASSERT(at == 17u);
    ((PBytes *)roots[46])->length = at;
    fields[P_PROTO_CODE] = roots[46];
    fields[P_PROTO_CONSTANTS] = roots[50];
    fields[P_PROTO_STACK] = p_fixnum(1);
    roots[51] = p_vm_make_proto(&p, fields);
    ASSERT(roots[51] && p_vm_verify_workspace(at, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[52] = p_bytes(&p, workspace_size);
    ASSERT(p_vm_verify(&p, roots[51], ((PBytes *)roots[52])->data,
                       workspace_size));
    roots[53] = p_vm_make_closure(&p, roots[51], roots[34]);
    ASSERT(roots[53]);
    ASSERT(p_vm_call(&vm, roots[53], 0u, NULL) == p_fixnum(77));

    /* The stage0 callback must see the entire expanded CALL window rooted,
     * including the last operand shifted above the old stack root count. */
    roots[55] = p_symbol_ptr(roots[2])->function;
    ASSERT(p_reserve(&p, sizeof(PClosure)));
    PClosure *interpreted_probe = (PClosure *)p_alloc(&p, P_CLOSURE,
                                                       sizeof(PClosure));
    ASSERT(interpreted_probe);
    interpreted_probe->name = p.nil;
    interpreted_probe->parameters = p.nil;
    interpreted_probe->body = p.nil;
    interpreted_probe->environment = p.nil;
    interpreted_probe->required_count = 2u;
    interpreted_probe->rest_parameter = p.nil;
    interpreted_probe->flags = 0u;
    roots[54] = (Value)interpreted_probe;
    p_symbol_ptr(roots[2])->function = roots[54];
    state.interpreted_apply = fallback_root_probe;
    interpreted_arguments_rooted = false;
    ASSERT(p_vm_call(&vm, roots[3], 0u, NULL) == p_fixnum(42));
    ASSERT(interpreted_arguments_rooted);

    /* Ordinary CALL operands are pushed by bytecode after the entry root
     * sync, so the dispatch path must register them before the interpreted
     * callback binds arguments or allocates. */
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 32u +
                     offsetof(PVector, data) + 3u * sizeof(Value) +
                     sizeof(PProto) + offsetof(PBytes, data) + 512u +
                     offsetof(PVector, data) + sizeof(PClosure)));
    roots[5] = p_bytes(&p, 32u);
    roots[6] = p_vector(&p, 3u, p.nil);
    ASSERT(roots[5] && roots[6]);
    p_vector_ptr(roots[6])->data[0] = roots[2];
    p_vector_ptr(roots[6])->data[1] = p_fixnum(19);
    p_vector_ptr(roots[6])->data[2] = p_fixnum(23);
    uint8_t *call_code = ((PBytes *)roots[5])->data;
    at = 0u;
    call_code[at++] = P_OP_FUNCTION; put_u32(call_code, &at, 0u);
    call_code[at++] = P_OP_CONST; put_u32(call_code, &at, 1u);
    call_code[at++] = P_OP_CONST; put_u32(call_code, &at, 2u);
    call_code[at++] = P_OP_CALL; put_u32(call_code, &at, 2u);
    call_code[at++] = P_OP_RETURN;
    ASSERT(at == 21u);
    ((PBytes *)roots[5])->length = at;
    fields[P_PROTO_CODE] = roots[5];
    fields[P_PROTO_CONSTANTS] = roots[6];
    fields[P_PROTO_STACK] = p_fixnum(3);
    roots[7] = p_vm_make_proto(&p, fields);
    ASSERT(roots[7] && p_vm_verify_workspace(at, &workspace_size));
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + workspace_size));
    roots[8] = p_bytes(&p, workspace_size);
    ASSERT(roots[8] && p_vm_verify(&p, roots[7],
                                   ((PBytes *)roots[8])->data,
                                   workspace_size));
    roots[9] = p_vector(&p, 0u, p.nil);
    ASSERT(roots[9]);
    roots[10] = p_vm_make_closure(&p, roots[7], roots[9]);
    ASSERT(roots[10]);
    interpreted_arguments_rooted = false;
    ASSERT(p_vm_call(&vm, roots[10], 0u, NULL) == p_fixnum(42));
    ASSERT(interpreted_arguments_rooted);
    state.interpreted_apply = NULL;
    p_symbol_ptr(roots[2])->function = roots[55];

    /* The emitted fallback symbol makes later global redefinitions visible. */
    Value minus = p_intern(&p, p.cl, "-", NULL);
    ASSERT(minus != 0u && p_type(p_symbol_ptr(minus)->function) == P_PRIMITIVE);
    p_symbol_ptr(roots[2])->function = p_symbol_ptr(minus)->function;
    ASSERT(p_vm_call(&vm, roots[3], 0u, NULL) == p_fixnum(-4));

    /* Operand references are checked even in unreachable bytecode. */
    ASSERT(p_reserve(&p, offsetof(PBytes, data) + 16u + sizeof(PProto) +
                     offsetof(PBytes, data) + workspace_size));
    roots[5] = p_bytes(&p, 16u);
    ASSERT(roots[5] != 0u);
    uint8_t *bad_code = ((PBytes *)roots[5])->data;
    at = 0u;
    bad_code[at++] = P_OP_JUMP; put_u32(bad_code, &at, 5u);
    bad_code[at++] = P_OP_CONST; put_u32(bad_code, &at, 99u);
    bad_code[at++] = P_OP_CONST; put_u32(bad_code, &at, 0u);
    bad_code[at++] = P_OP_RETURN;
    ((PBytes *)roots[5])->length = at;
    fields[P_PROTO_CODE] = roots[5];
    roots[4] = p_vm_make_proto(&p, fields);
    ASSERT(roots[4] != 0u);
    ASSERT(p_vm_verify_workspace(at, &workspace_size));
    roots[3] = p_bytes(&p, workspace_size);
    ASSERT(roots[3] != 0u);
    p_clear_error(&p);
    ASSERT(!p_vm_verify(&p, roots[4], ((PBytes *)roots[3])->data,
                        workspace_size));
    ASSERT(p.error.code == P_ERR_IMAGE);
    p_clear_error(&p);

    p_root_restore(&p, saved);
    p_vm_destroy(&vm);
    p_state_destroy(&state);
    p_destroy(&p);
}
