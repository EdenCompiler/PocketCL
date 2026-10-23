#include "exec.h"

#include <string.h>

bool p_exec_seed_core_symbols(Pocket *p)
{
    static const char *const operators[] = {
        "QUOTE", "FUNCTION", "IF", "SETQ", "PROGN", "LET", "LET*",
        "FLET", "LABELS", "BLOCK", "RETURN-FROM", "TAGBODY", "GO",
        "CATCH", "THROW", "UNWIND-PROTECT", "MULTIPLE-VALUE-CALL",
        "MULTIPLE-VALUE-PROG1", "THE", "LOCALLY", "MACROLET",
        "SYMBOL-MACROLET", "EVAL-WHEN"
    };
    static const char *const syntax[] = {
        "DECLARE", "LAMBDA", "&REST", "&OPTIONAL", "&KEY",
        "&ALLOW-OTHER-KEYS", "&AUX", "&BODY", "&WHOLE", "&ENVIRONMENT",
        "SPECIAL", "IGNORE", "IGNORABLE", "INLINE", "NOTINLINE", "TYPE",
        "FTYPE", "OPTIMIZE", "DYNAMIC-EXTENT", "SPEED", "SPACE", "DEBUG",
        "SAFETY", "COMPILATION-SPEED"
    };
    static const char *const evaluator_functions[] = { "FUNCALL", "APPLY", "VALUES" };
    if (!p_reserve(p, 65536u)) return false;
    for (size_t i = 0u; i < sizeof(operators) / sizeof(operators[0]); ++i) {
        Value symbol = p_intern(p, p->cl, operators[i], NULL);
        if (symbol == 0u || !p_export(p, p->cl, symbol)) return false;
        p_symbol_ptr(symbol)->flags |= P_SPECIAL_OPERATOR;
    }
    for (size_t i = 0u; i < sizeof(syntax) / sizeof(syntax[0]); ++i) {
        Value symbol = p_intern(p, p->cl, syntax[i], NULL);
        if (symbol == 0u || !p_export(p, p->cl, symbol)) return false;
    }
    for (size_t i = 0u; i < sizeof(evaluator_functions) / sizeof(evaluator_functions[0]); ++i) {
        Value symbol = p_intern(p, p->cl, evaluator_functions[i], NULL);
        if (symbol == 0u || !p_export(p, p->cl, symbol)) return false;
    }
    return true;
}

bool p_state_init(PState *state, Pocket *p)
{
    if (state == NULL || p == NULL) return false;
    memset(state, 0, sizeof(*state));
    state->p = p;
    state->mv_count = 1u;
    state->mv[0] = p->nil;
    state->max_call_depth = P_EVAL_DEPTH_LIMIT;
    state->saved_root_count = p_root_save(p);
    if (!p_roots(p, state->mv, P_MULTIPLE_VALUES_LIMIT) ||
        !p_roots(p, &state->current_env, 1u) ||
        !p_roots(p, &state->control_owner, 1u) ||
        !p_roots(p, state->roots, P_EVAL_ROOT_LIMIT) ||
        !p_roots(p, &state->tail_function, 1u) ||
        !p_roots(p, state->tail_arguments, 512u) ||
        !p_exec_seed_core_symbols(p)) {
        p_root_restore(p, state->saved_root_count);
        memset(state, 0, sizeof(*state));
        return false;
    }
    state->initialized = true;
    return true;
}

void p_state_destroy(PState *state)
{
    if (state == NULL || state->p == NULL) return;
    Pocket *p = state->p;
    p_root_restore(p, state->saved_root_count);
    memset(state, 0, sizeof(*state));
}

Value p_exec_env_binding(Value environment, PEnvNamespace space, Value name)
{
    for (Value env = environment; p_type(env) == P_RECORD;
         env = ((PRecord *)env)->data[0]) {
        if (((PRecord *)env)->type != P_EXEC_ENV_TAG ||
            ((PRecord *)env)->length < P_EXEC_ENV_SLOTS) break;
        Value cursor = ((PRecord *)env)->data[(size_t)space + 1u];
        while (p_type(cursor) == P_CONS) {
            Value binding = p_cons_ptr(cursor)->car;
            if (p_type(binding) == P_CONS && p_cons_ptr(binding)->car == name)
                return binding;
            cursor = p_cons_ptr(cursor)->cdr;
        }
    }
    return 0u;
}

Value p_env_create(PState *state, Value parent)
{
    Pocket *p = state->p;
    Value roots[2] = { parent, 0u };
    size_t saved = p_root_save(p);
    if (!p_roots(p, roots, 2u) ||
        !p_reserve(p, offsetof(PRecord, data) + P_EXEC_ENV_SLOTS * sizeof(Value))) {
        p_root_restore(p, saved);
        return 0u;
    }
    roots[1] = p_record(p, P_EXEC_ENV_TAG, P_EXEC_ENV_SLOTS);
    if (roots[1] != 0u) ((PRecord *)roots[1])->data[0] = roots[0];
    Value result = roots[1];
    p_root_restore(p, saved);
    return result;
}

Value p_env_lookup(PState *state, Value environment,
                   PEnvNamespace space, Value name)
{
    (void)state;
    Value binding = p_exec_env_binding(environment, space, name);
    return binding == 0u ? 0u : p_cons_ptr(binding)->cdr;
}

bool p_env_bind(PState *state, Value environment, PEnvNamespace space,
               Value name, Value value, bool replace)
{
    Pocket *p = state->p;
    if (p_type(environment) != P_RECORD ||
        ((PRecord *)environment)->type != P_EXEC_ENV_TAG) {
        (void)p_error(p, P_ERR_TYPE, "expected lexical environment");
        return false;
    }
    Value roots[5] = { environment, name, value, 0u, 0u };
    size_t saved = p_root_save(p);
    if (!p_roots(p, roots, 5u) || !p_reserve(p, 2u * sizeof(PCons))) {
        p_root_restore(p, saved);
        return false;
    }
    Value existing = replace ? p_exec_env_binding(roots[0], space, roots[1]) : 0u;
    if (existing != 0u) {
        p_cons_ptr(existing)->cdr = roots[2];
        p_root_restore(p, saved);
        return true;
    }
    roots[3] = p_cons(p, roots[1], roots[2]);
    if (roots[3] == 0u) { p_root_restore(p, saved); return false; }
    PRecord *record = (PRecord *)roots[0];
    roots[4] = p_cons(p, roots[3], record->data[(size_t)space + 1u]);
    if (roots[4] == 0u) { p_root_restore(p, saved); return false; }
    ((PRecord *)roots[0])->data[(size_t)space + 1u] = roots[4];
    p_root_restore(p, saved);
    return true;
}

bool p_env_set(PState *state, Value environment, PEnvNamespace space,
               Value name, Value value)
{
    Value binding = p_exec_env_binding(environment, space, name);
    if (binding == 0u) return false;
    if (space == P_ENV_VALUE && p_cons_ptr(binding)->cdr == P_UNBOUND) {
        if (p_type(name) == P_SYMBOL) p_symbol_ptr(name)->value = value;
        else (void)p_error(state->p, P_ERR_TYPE, "special binding name must be a symbol");
        return true;
    }
    p_cons_ptr(binding)->cdr = value;
    return true;
}

bool p_exec_env_bind_special_marker(PState *state, Value environment,
                                    Value name)
{
    return p_env_bind(state, environment, P_ENV_VALUE, name, P_UNBOUND, false);
}

Value p_macro_function(PState *state, Value environment, Value name)
{
    (void)state;
    if (p_type(name) != P_SYMBOL) return 0u;
    /* Search one contour at a time. A nearer FLET/LABELS binding suppresses
     * an outer macro; searching the complete macro chain first would change
     * the meaning of a call in that lexical scope. No Lisp runs here. */
    for (Value env = environment; p_type(env) == P_RECORD;
         env = ((PRecord *)env)->data[0]) {
        PRecord *record = (PRecord *)env;
        if (record->type != P_EXEC_ENV_TAG || record->length < P_EXEC_ENV_SLOTS) break;
        Value functions = record->data[P_ENV_FUNCTION + 1u];
        for (Value cursor = functions; p_type(cursor) == P_CONS;
             cursor = p_cons_ptr(cursor)->cdr) {
            Value binding = p_cons_ptr(cursor)->car;
            if (p_type(binding) == P_CONS && p_cons_ptr(binding)->car == name) return 0u;
        }
        Value macros = record->data[P_ENV_MACRO + 1u];
        for (Value cursor = macros; p_type(cursor) == P_CONS;
             cursor = p_cons_ptr(cursor)->cdr) {
            Value binding = p_cons_ptr(cursor)->car;
            if (p_type(binding) == P_CONS && p_cons_ptr(binding)->car == name)
                return p_cons_ptr(binding)->cdr;
        }
    }
    Value function = p_symbol_ptr(name)->function;
    if (p_type(function) == P_CLOSURE &&
        ((((PClosure *)function)->flags & P_EVAL_MACRO) != 0u ||
         (p_symbol_ptr(name)->flags & P_MACRO) != 0u)) return function;
    return 0u;
}
