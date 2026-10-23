#include "compiler_prim.h"

static Value constant_symbol_p(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return p_type(argv[0]) == P_SYMBOL &&
           (p_symbol_ptr(argv[0])->flags & P_CONSTANT) != 0u
        ? state->p->truth : state->p->nil;
}

static Value builtin_function_p(PState *state, size_t argc, const Value *argv)
{
    static const char *const names[] = {"+", "-", "<", "=", "CONS", "CAR", "CDR"};
    (void)argc;
    /* Lisp chooses folding policy. This exposes only the same identity guard
     * used by VM fast paths, so a changed global function remains observable. */
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (argv[0] == p_find_symbol(state->p, state->p->cl, names[i], NULL) &&
            p_is_builtin_primitive(state->p, argv[0], names[i])) return state->p->truth;
    return state->p->nil;
}

static Value object_kind(PState *state, size_t argc, const Value *argv)
{
    (void)state;
    (void)argc;
    if (p_is_fixnum(argv[0])) return p_fixnum(-1);
    if (p_is_character(argv[0])) return p_fixnum(-2);
    return p_fixnum(p_is_pointer(argv[0]) ? (int64_t)p_type(argv[0]) : -3);
}

static Value fixnum_bits(PState *state, size_t argc, const Value *argv)
{
    (void)state;
    (void)argc;
    (void)argv;
    return p_fixnum(p_fixnum_bits());
}

static Value eq_hash(PState *state, size_t argc, const Value *argv)
{
    /* Unsigned mixing is defined on both targets. Thirty positive bits fit
     * either fixnum width. This identity hash is only a lookup aid: Lisp
     * controls traversal, structural equality, and deterministic output. */
    uint64_t bits = (uint64_t)argv[0];
    (void)state;
    (void)argc;
    bits ^= bits >> 30u;
    bits *= UINT64_C(0xbf58476d1ce4e5b9);
    bits ^= bits >> 27u;
    bits *= UINT64_C(0x94d049bb133111eb);
    bits ^= bits >> 31u;
    return p_fixnum((int64_t)(bits & UINT64_C(0x3fffffff)));
}

static Value install_hooks(PState *state, size_t argc, const Value *argv)
{
    Value hook = argv[0];
    (void)argc;
    if (hook != state->p->nil && p_type(hook) != P_CLOSURE && p_type(hook) != P_PRIMITIVE)
        return p_error(state->p, P_ERR_TYPE, "error hook must be a function or NIL");
    state->p->error_hook = hook;
    return hook;
}

static Value unhandled_error(PState *state, size_t argc, const Value *argv)
{
    char message[256];
    size_t written;
    (void)argc;
    if (p_type(argv[1]) != P_STRING)
        return p_error(state->p, P_ERR_TYPE, "unhandled condition report must be a string");
    if (!p_string_to_utf8(state->p, argv[1], message, sizeof(message) - 1u, &written)) return 0;
    message[written] = '\0';
    (void)p_error(state->p, P_ERR_CONTROL, message);
    state->p->error.datum = argv[0];
    state->p->error.signaled = true;
    return 0;
}

bool p_install_compiler_primitives(PState *state)
{
    return p_add_primitive(state, "%OBJECT-KIND", false, object_kind, NULL, 1, 1, 0) &&
           p_add_primitive(state, "%CONSTANT-SYMBOL-P", false, constant_symbol_p, NULL, 1, 1, 0) &&
           p_add_primitive(state, "%BUILTIN-FUNCTION-P", false, builtin_function_p, NULL, 1, 1, 0) &&
           p_add_primitive(state, "%FIXNUM-BITS", false, fixnum_bits, NULL, 0, 0, 0) &&
           p_add_primitive(state, "%EQ-HASH", false, eq_hash, NULL, 1, 1, 0) &&
           p_add_primitive(state, "%INSTALL-HOOKS", false, install_hooks, NULL, 1, 1, 0) &&
           p_add_primitive(state, "%UNHANDLED-ERROR", false, unhandled_error, NULL, 2, 2, 0);
}
