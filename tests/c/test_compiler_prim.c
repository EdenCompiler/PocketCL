#include "test.h"

#include "compiler_prim.h"
#include "exec.h"

static PPrimitive *find_sys_primitive(Pocket *p, const char *name)
{
    Value symbol = p_find_symbol(p, p->sys, name, NULL);
    ASSERT(symbol != p->nil);
    ASSERT(p_type(p_symbol_ptr(symbol)->function) == P_PRIMITIVE);
    return (PPrimitive *)p_symbol_ptr(symbol)->function;
}

static Value call_primitive(PState *state, PPrimitive *primitive,
                            size_t argc, const Value *argv)
{
    ASSERT(primitive->min_arity <= argc);
    ASSERT(primitive->max_arity == P_EVAL_PRIMITIVE_VARIADIC ||
           argc <= primitive->max_arity);
    return primitive->entry(state, argc, argv);
}

void test_compiler_prim(void)
{
    Pocket p;
    PState state;
    PPrimitive *kind, *bits, *hash, *hooks, *builtin, *constant;
    Value args[2], result, package_symbol, plus, less;
    Value original_plus_function, hash_input;

    ASSERT(p_init(&p, 1024u * 1024u));
    ASSERT(p_state_init(&state, &p));
    ASSERT(p_install_primitives(&state));
    ASSERT(p_install_compiler_primitives(&state));

    kind = find_sys_primitive(&p, "%OBJECT-KIND");
    bits = find_sys_primitive(&p, "%FIXNUM-BITS");
    hash = find_sys_primitive(&p, "%EQ-HASH");
    hooks = find_sys_primitive(&p, "%INSTALL-HOOKS");
    builtin = find_sys_primitive(&p, "%BUILTIN-FUNCTION-P");
    constant = find_sys_primitive(&p, "%CONSTANT-SYMBOL-P");
    args[0] = p.nil;
    ASSERT(call_primitive(&state, constant, 1u, args) == p.truth);
    args[0] = p.truth;
    ASSERT(call_primitive(&state, constant, 1u, args) == p.truth);
    args[0] = p_fixnum(42);
    ASSERT(call_primitive(&state, constant, 1u, args) == p.nil);
    args[0] = p_find_symbol(&p, p.cl, "+", NULL);
    ASSERT(call_primitive(&state, constant, 1u, args) == p.nil);

    args[0] = p_fixnum(17);
    result = call_primitive(&state, kind, 1u, args);
    ASSERT(p_is_fixnum(result) && p_fixnum_value(result) == -1);
    args[0] = p_character(0x03bbu);
    result = call_primitive(&state, kind, 1u, args);
    ASSERT(p_is_fixnum(result) && p_fixnum_value(result) == -2);
    args[0] = p.nil;
    result = call_primitive(&state, kind, 1u, args);
    ASSERT(p_is_fixnum(result) && p_fixnum_value(result) == P_SYMBOL);
    args[0] = P_UNBOUND;
    result = call_primitive(&state, kind, 1u, args);
    ASSERT(p_is_fixnum(result) && p_fixnum_value(result) == -3);
    result = call_primitive(&state, bits, 0u, NULL);
    ASSERT(p_is_fixnum(result) &&
           (unsigned)p_fixnum_value(result) == p_fixnum_bits());

    args[0] = p_fixnum(12345);
    result = call_primitive(&state, hash, 1u, args);
    ASSERT(p_is_fixnum(result));
    ASSERT(p_fixnum_value(result) >= 0);
    ASSERT(p_fixnum_value(result) <= INT64_C(0x3fffffff));
    ASSERT(call_primitive(&state, hash, 1u, args) == result);
    args[0] = p_character(0x10ffffu);
    result = call_primitive(&state, hash, 1u, args);
    ASSERT(p_is_fixnum(result) && p_fixnum_value(result) >= 0 &&
           p_fixnum_value(result) <= INT64_C(0x3fffffff));
    args[0] = p.nil;
    result = call_primitive(&state, hash, 1u, args);
    ASSERT(p_is_fixnum(result) && p_fixnum_value(result) >= 0 &&
           p_fixnum_value(result) <= INT64_C(0x3fffffff));
    hash_input = args[0];
    ASSERT(call_primitive(&state, hash, 1u, &hash_input) == result);

    /* NIL clears the native error hook; primitive functions are valid hooks,
     * while other Lisp values report a type error and leave it unchanged. */
    args[0] = p.nil;
    ASSERT(call_primitive(&state, hooks, 1u, args) == p.nil);
    ASSERT(p.error_hook == p.nil);
    args[0] = p.truth;
    (void)call_primitive(&state, hooks, 1u, args);
    ASSERT(p.error.code == P_ERR_TYPE);
    ASSERT(p.error_hook == p.nil);
    p_clear_error(&p);
    args[0] = p_symbol_ptr(p_find_symbol(&p, p.cl, "+", NULL))->function;
    ASSERT(p_type(args[0]) == P_PRIMITIVE);
    ASSERT(call_primitive(&state, hooks, 1u, args) == args[0]);
    ASSERT(p.error_hook == args[0]);
    args[0] = p.nil;
    ASSERT(call_primitive(&state, hooks, 1u, args) == p.nil);
    ASSERT(p.error_hook == p.nil);

    /* The VM may inline only the original builtin entry. Replacing + with
     * another primitive must immediately disable the arithmetic fast path. */
    plus = p_find_symbol(&p, p.cl, "+", NULL);
    less = p_find_symbol(&p, p.cl, "<", NULL);
    ASSERT(plus != p.nil && less != p.nil);
    ASSERT(p_is_builtin_primitive(&p, plus, "+"));
    ASSERT(call_primitive(&state, builtin, 1u, &plus) == p.truth);
    original_plus_function = p_symbol_ptr(plus)->function;
    p_symbol_ptr(plus)->function = p_symbol_ptr(less)->function;
    ASSERT(!p_is_builtin_primitive(&p, plus, "+"));
    ASSERT(call_primitive(&state, builtin, 1u, &plus) == p.nil);
    p_symbol_ptr(plus)->function = original_plus_function;
    ASSERT(p_is_builtin_primitive(&p, plus, "+"));

    package_symbol = p_make_symbol(&p, "+");
    ASSERT(package_symbol != 0u);
    ASSERT(!p_is_builtin_primitive(&p, package_symbol, "+"));
    ASSERT(call_primitive(&state, builtin, 1u, &package_symbol) == p.nil);
    args[0] = p_fixnum(1);
    ASSERT(call_primitive(&state, builtin, 1u, args) == p.nil);

    /* A report exactly as wide as the C buffer must leave room for its NUL.
     * Already-signaled errors suppress a second pass through Lisp handlers. */
    {
        PPrimitive *unhandled = find_sys_primitive(&p, "%UNHANDLED-ERROR");
        PPrimitive *raw_error = find_sys_primitive(&p, "%ERROR");
        uint32_t text[256];
        for (size_t i = 0; i < 256u; ++i) text[i] = 'x';
        ASSERT(p_reserve(&p, 2u * (sizeof(PString) + sizeof(text))));
        args[0] = p.nil;
        args[1] = p_string(&p, text, 255u);
        ASSERT(args[1] != 0u);
        ASSERT(call_primitive(&state, unhandled, 2u, args) == 0u);
        ASSERT(p.error.code == P_ERR_CONTROL && p.error.signaled);
        p_clear_error(&p);
        args[1] = p_string(&p, text, 256u);
        ASSERT(args[1] != 0u);
        ASSERT(call_primitive(&state, unhandled, 2u, args) == 0u);
        ASSERT(p.error.code == P_ERR_RANGE && !p.error.signaled);
        p_clear_error(&p);
        ASSERT(call_primitive(&state, raw_error, 1u, args + 1) == 0u);
        ASSERT(p.error.code == P_ERR_RANGE && !p.error.signaled);
        p_clear_error(&p);
    }
    p_state_destroy(&state);
    p_destroy(&p);
}
