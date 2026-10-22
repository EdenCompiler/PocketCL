#include "eval.h"
#include "heap_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#define ENV_TAG p_fixnum(-73)
#define ENV_SLOTS 7u
#define CONTROL_LIMIT 128u
#define DYNAMIC_LIMIT 256u
#define ARGUMENT_LIMIT 256u

typedef enum { CONTROL_CATCH, CONTROL_BLOCK, CONTROL_TAGBODY } ControlKind;
typedef enum { DYNAMIC_SPECIAL, DYNAMIC_CLEANUP } DynamicKind;

typedef struct {
    jmp_buf jump;
    ControlKind kind;
    size_t token_slot, environment_slot, dynamic_base, target_index, root_base;
    size_t pocket_root_base, call_depth, control_index;
    bool saved_tail_pending;
    Value transfer_values[P_MULTIPLE_VALUES_LIMIT];
    size_t transfer_count;
} ControlFrame;

typedef struct {
    DynamicKind kind;
    size_t symbol_slot, old_value_slot, forms_slot, env_slot;
    size_t root_base;
} DynamicFrame;

typedef struct {
    ControlFrame controls[CONTROL_LIMIT];
    DynamicFrame dynamics[DYNAMIC_LIMIT];
    size_t control_count, dynamic_count;
} EvalInternals;

static EvalInternals *internals(PState *state) {
    return (EvalInternals *)((PBytes *)state->control_owner)->data;
}

static Value symbol_name(Value symbol) {
    return p_type(symbol) == P_SYMBOL ? p_symbol_ptr(symbol)->name : 0u;
}

static bool symbol_named(Pocket *p, Value symbol, const char *name) {
    /* Package interning guarantees a single canonical CL symbol per name.
     * Checking its home package and pname avoids a package-table walk on every
     * evaluator dispatch (especially important in million-call tail loops). */
    return p_type(symbol) == P_SYMBOL && p_symbol_ptr(symbol)->home == p->cl &&
           p_string_equals_ascii(symbol_name(symbol), name);
}

static bool keyword_named(Pocket *p, Value symbol, const char *name) {
    return p_type(symbol) == P_SYMBOL && p_symbol_ptr(symbol)->home == p->keyword &&
           p_string_equals_ascii(symbol_name(symbol), name);
}

static bool seed_core_symbols(Pocket *p) {
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
        int status = 0;
        Value symbol = p_intern(p, p->cl, operators[i], &status);
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

static bool proper_list(Pocket *p, Value list, size_t *length) {
    size_t count = 0u;
    while (p_type(list) == P_CONS) {
        if (++count > p->heap_size / P_MIN_BLOCK) return false;
        list = p_cons_ptr(list)->cdr;
    }
    if (list != p->nil) return false;
    if (length != NULL) *length = count;
    return true;
}

static Value list_nth(Pocket *p, Value list, size_t index) {
    while (index-- != 0u && p_type(list) == P_CONS)
        list = p_cons_ptr(list)->cdr;
    return p_type(list) == P_CONS ? p_cons_ptr(list)->car : p->nil;
}

static Value eval_form(PState *state, Value form, Value environment, bool tail_position);
static Value eval_sequence(PState *state, Value body, Value environment,
                          bool tail_position);
static Value apply_values(PState *state, Value function, size_t argc,
                          const Value *argv);
static Value global_function_designator(PState *state, Value designator);
static bool block_tail_unsafe(PState *state, Value tree, Value name,
                              Value environment);
static Value expand_lambda_list(PState *state, Value parameters, Value body,
                                bool macro, Value environment);
static Value make_closure_at_safe_point(PState *state, Value params, Value body,
                                        Value environment, bool macro, Value name);
static bool is_declared_special(Pocket *p, Value body, Value symbol);

Value p_env_create(PState *state, Value parent) {
    Pocket *p = state->p;
    Value roots[2] = { parent, 0u };
    size_t saved = p_root_save(p);
    if (!p_roots(p, roots, 2u) || !p_reserve(p, offsetof(PRecord, data) + ENV_SLOTS * sizeof(Value))) {
        p_root_restore(p, saved);
        return 0u;
    }
    roots[1] = p_record(p, ENV_TAG, ENV_SLOTS);
    if (roots[1] != 0u) ((PRecord *)roots[1])->data[0] = roots[0];
    Value result = roots[1];
    p_root_restore(p, saved);
    return result;
}

static Value find_binding(Value environment, PEnvNamespace space, Value name) {
    for (Value env = environment; p_type(env) == P_RECORD;
         env = ((PRecord *)env)->data[0]) {
        if (((PRecord *)env)->type != ENV_TAG || ((PRecord *)env)->length < ENV_SLOTS)
            break;
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

static Value find_local_binding(Value environment, PEnvNamespace space, Value name) {
    if (p_type(environment) != P_RECORD ||
        ((PRecord *)environment)->type != ENV_TAG ||
        ((PRecord *)environment)->length < ENV_SLOTS)
        return 0u;
    Value cursor = ((PRecord *)environment)->data[(size_t)space + 1u];
    while (p_type(cursor) == P_CONS) {
        Value binding = p_cons_ptr(cursor)->car;
        if (p_type(binding) == P_CONS && p_cons_ptr(binding)->car == name)
            return binding;
        cursor = p_cons_ptr(cursor)->cdr;
    }
    return 0u;
}

static Value lookup_symbol_macro(PState *state, Value environment, Value name) {
    for (Value env = environment; p_type(env) == P_RECORD &&
         ((PRecord *)env)->type == ENV_TAG; env = ((PRecord *)env)->data[0]) {
        if (find_local_binding(env, P_ENV_VALUE, name) != 0u) return 0u;
        Value expansion = find_local_binding(env, P_ENV_SYMBOL_MACRO, name);
        if (expansion != 0u) return p_cons_ptr(expansion)->cdr;
    }
    (void)state;
    return 0u;
}

/* These two helpers are used only after their caller has reserved the whole
 * closure environment and binding list in one safe-point operation. Keeping
 * them allocation-only lets GC-stress retain the same semantics without
 * collecting between each argument binding. */
static Value env_create_reserved(PState *state, Value parent) {
    Value env = p_record(state->p, ENV_TAG, ENV_SLOTS);
    if (env != 0u) ((PRecord *)env)->data[0] = parent;
    return env;
}

static bool env_bind_reserved(PState *state, Value environment,
                              PEnvNamespace space, Value name, Value value) {
    Pocket *p = state->p;
    Value binding = p_cons(p, name, value);
    if (binding == 0u) return false;
    Value *slot = &((PRecord *)environment)->data[(size_t)space + 1u];
    Value list = p_cons(p, binding, *slot);
    if (list == 0u) return false;
    ((PRecord *)environment)->data[(size_t)space + 1u] = list;
    return true;
}

static bool env_bind_special_marker(PState *state, Value environment, Value name) {
    return p_env_bind(state, environment, P_ENV_VALUE, name, P_UNBOUND, false);
}

Value p_env_lookup(PState *state, Value environment,
                   PEnvNamespace space, Value name) {
    (void)state;
    Value binding = find_binding(environment, space, name);
    return binding == 0u ? 0u : p_cons_ptr(binding)->cdr;
}

bool p_env_bind(PState *state, Value environment, PEnvNamespace space,
               Value name, Value value, bool replace) {
    Pocket *p = state->p;
    if (p_type(environment) != P_RECORD || ((PRecord *)environment)->type != ENV_TAG) {
        (void)p_error(p, P_ERR_TYPE, "expected lexical environment");
        return false;
    }
    Value roots[5] = { environment, name, value, 0u, 0u };
    size_t saved = p_root_save(p);
    if (!p_roots(p, roots, 5u) || !p_reserve(p, 2u * sizeof(PCons))) {
        p_root_restore(p, saved);
        return false;
    }
    Value existing = replace ? find_binding(roots[0], space, roots[1]) : 0u;
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
    record = (PRecord *)roots[0];
    record->data[(size_t)space + 1u] = roots[4];
    p_root_restore(p, saved);
    return true;
}

bool p_env_set(PState *state, Value environment, PEnvNamespace space,
               Value name, Value value) {
    Value binding = find_binding(environment, space, name);
    if (binding == 0u) return false;
    /* P_UNBOUND in a lexical value cell is the marker for a DECLARE SPECIAL
     * shadow: assignment goes to the symbol's dynamic value cell. */
    if (space == P_ENV_VALUE && p_cons_ptr(binding)->cdr == P_UNBOUND) {
        if (p_type(name) == P_SYMBOL) p_symbol_ptr(name)->value = value;
        else (void)p_error(state->p, P_ERR_TYPE, "special binding name must be a symbol");
        return true;
    }
    p_cons_ptr(binding)->cdr = value;
    return true;
}

void p_eval_init(PState *state, Pocket *p) {
    memset(state, 0, sizeof(*state));
    state->p = p;
    state->mv_count = 1u;
    state->max_call_depth = P_EVAL_DEPTH_LIMIT;
    state->saved_root_count = p_root_save(p);
    if (!p_roots(p, state->mv, P_MULTIPLE_VALUES_LIMIT) ||
        !p_roots(p, &state->current_env, 1u) ||
        !p_roots(p, &state->control_owner, 1u) ||
        !p_roots(p, state->roots, P_EVAL_ROOT_LIMIT) ||
        !p_roots(p, &state->tail_function, 1u) ||
        !p_roots(p, state->tail_arguments, 512u) ||
        !p_reserve(p, sizeof(EvalInternals) + sizeof(PBytes))) {
        p_root_restore(p, state->saved_root_count);
        return;
    }
    if (!seed_core_symbols(p)) {
        p_root_restore(p, state->saved_root_count);
        return;
    }
    state->control_owner = (Value)p_bytes(p, sizeof(EvalInternals));
    if (state->control_owner == 0u) {
        p_root_restore(p, state->saved_root_count);
        return;
    }
    state->control = ((PBytes *)state->control_owner)->data;
    state->control_capacity = CONTROL_LIMIT;
    state->initialized = true;
}

void p_eval_destroy(PState *state) {
    if (state == NULL || state->p == NULL) return;
    p_root_restore(state->p, state->saved_root_count);
    state->initialized = false;
}

static size_t root_keep(PState *state, Value value) {
    if (state->root_used >= P_EVAL_ROOT_LIMIT) {
        (void)p_error(state->p, P_ERR_STACK, "stage-0 temporary root stack exhausted");
        return SIZE_MAX;
    }
    size_t slot = state->root_used++;
    state->roots[slot] = value;
    return slot;
}

static void root_rewind(PState *state, size_t saved) {
    while (state->root_used > saved)
        state->roots[--state->root_used] = 0u;
}

static ControlFrame *control_push(PState *state, ControlKind kind, Value token) {
    EvalInternals *inner = internals(state);
    if (inner->control_count >= state->control_capacity) {
        (void)p_error(state->p, P_ERR_STACK, "stage-0 control stack exhausted");
        return NULL;
    }
    ControlFrame *frame = &inner->controls[inner->control_count++];
    memset(frame, 0, sizeof(*frame));
    frame->kind = kind;
    frame->dynamic_base = inner->dynamic_count;
    frame->root_base = state->root_used;
    frame->pocket_root_base = p_root_save(state->p);
    frame->call_depth = state->call_depth;
    frame->control_index = inner->control_count - 1u;
    frame->saved_tail_pending = state->tail_pending;
    frame->token_slot = root_keep(state, token);
    frame->environment_slot = root_keep(state, state->current_env);
    if (frame->token_slot == SIZE_MAX || frame->environment_slot == SIZE_MAX) {
        root_rewind(state, frame->root_base);
        --inner->control_count;
        return NULL;
    }
    return frame;
}

/* setjmp/longjmp skips evaluator C frames, including their registered C root
 * ranges. Restore those stacks before evaluating any cleanup code or collecting. */
static void recover_control_jump(PState *state, ControlFrame *frame) {
    EvalInternals *inner = internals(state);
    p_root_restore(state->p, frame->pocket_root_base);
    /* Transfer values live in the stable control frame so dynamic unwind can
     * discard temporary evaluator roots without erasing the values being
     * returned. Re-register them after restoring the catcher’s root mark. */
    (void)p_roots(state->p, frame->transfer_values, frame->transfer_count);
    state->root_used = frame->root_base + 2u;
    state->call_depth = frame->call_depth;
    state->current_env = state->roots[frame->environment_slot];
    state->tail_pending = frame->saved_tail_pending;
    inner->control_count = frame->control_index + 1u;
}

static void control_pop(PState *state, ControlFrame *frame) {
    EvalInternals *inner = internals(state);
    if (inner->control_count != 0u &&
        frame == &inner->controls[inner->control_count - 1u]) {
        root_rewind(state, frame->root_base);
        --inner->control_count;
    }
}

static ControlFrame *find_control(PState *state, ControlKind kind, Value token) {
    EvalInternals *inner = internals(state);
    for (size_t i = inner->control_count; i > 0u; --i) {
        ControlFrame *frame = &inner->controls[i - 1u];
        if (frame->kind == kind &&
            (token == 0u || state->roots[frame->token_slot] == token))
            return frame;
    }
    return NULL;
}

static bool save_transfer_values(PState *state, ControlFrame *frame) {
    frame->transfer_count = state->mv_count;
    if (frame->transfer_count > P_MULTIPLE_VALUES_LIMIT)
        frame->transfer_count = P_MULTIPLE_VALUES_LIMIT;
    memcpy(frame->transfer_values, state->mv,
           frame->transfer_count * sizeof(Value));
    return p_roots(state->p, frame->transfer_values, frame->transfer_count);
}

static void restore_transfer_values(PState *state, ControlFrame *frame) {
    state->mv_count = frame->transfer_count;
    for (size_t i = 0u; i < frame->transfer_count; ++i)
        state->mv[i] = frame->transfer_values[i];
}

static bool dynamic_push(PState *state, DynamicKind kind, Value a, Value b,
                         Value c, Value d) {
    EvalInternals *inner = internals(state);
    if (inner->dynamic_count >= DYNAMIC_LIMIT) {
        (void)p_error(state->p, P_ERR_STACK, "stage-0 dynamic state stack exhausted");
        return false;
    }
    DynamicFrame *frame = &inner->dynamics[inner->dynamic_count++];
    memset(frame, 0, sizeof(*frame));
    frame->kind = kind;
    frame->root_base = state->root_used;
    if (kind == DYNAMIC_SPECIAL) {
        frame->symbol_slot = root_keep(state, a);
        frame->old_value_slot = root_keep(state, b);
    } else {
        frame->forms_slot = root_keep(state, c);
        frame->env_slot = root_keep(state, d);
    }
    if (state->p->error.code != P_OK) {
        root_rewind(state, frame->root_base);
        --inner->dynamic_count;
        return false;
    }
    return true;
}

static bool unwind_dynamic(PState *state, size_t target_count) {
    EvalInternals *inner = internals(state);
    while (inner->dynamic_count > target_count) {
        DynamicFrame *frame = &inner->dynamics[--inner->dynamic_count];
        if (frame->kind == DYNAMIC_SPECIAL) {
            Value symbol = state->roots[frame->symbol_slot];
            if (p_type(symbol) == P_SYMBOL)
                p_symbol_ptr(symbol)->value = state->roots[frame->old_value_slot];
        } else {
            Value saved_values[P_MULTIPLE_VALUES_LIMIT] = { 0u };
            size_t saved_count = state->mv_count;
            if (saved_count > P_MULTIPLE_VALUES_LIMIT)
                saved_count = P_MULTIPLE_VALUES_LIMIT;
            size_t roots_saved = p_root_save(state->p);
            if (!p_roots(state->p, saved_values, P_MULTIPLE_VALUES_LIMIT))
                return false;
            memcpy(saved_values, state->mv, saved_count * sizeof(Value));
            Value forms = state->roots[frame->forms_slot];
            Value env = state->roots[frame->env_slot];
            (void)eval_sequence(state, forms, env, false);
            p_root_restore(state->p, roots_saved);
            if (state->p->error.code != P_OK) {
                root_rewind(state, frame->root_base);
                return false;
            }
            state->mv_count = saved_count;
            memcpy(state->mv, saved_values, saved_count * sizeof(Value));
        }
        root_rewind(state, frame->root_base);
    }
    return true;
}

Value p_make_closure(PState *state, Value parameters, Value body,
                     Value environment, bool macro, Value name) {
    Pocket *p = state->p;
    size_t count = 0u;
    Value cursor = parameters, rest = 0u;
    Value marker = p_find_symbol(p, p->cl, "&REST", NULL);
    bool saw_rest = false;
    static const char *const unsupported_markers[] = {
        "&OPTIONAL", "&KEY", "&ALLOW-OTHER-KEYS", "&AUX", "&BODY",
        "&WHOLE", "&ENVIRONMENT"
    };
    while (p_type(cursor) == P_CONS) {
        Value parameter = p_cons_ptr(cursor)->car;
        if (parameter == marker) {
            if (saw_rest || p_type(p_cons_ptr(cursor)->cdr) != P_CONS) break;
            cursor = p_cons_ptr(cursor)->cdr;
            rest = p_cons_ptr(cursor)->car;
            cursor = p_cons_ptr(cursor)->cdr;
            saw_rest = true;
            break;
        }
        bool unsupported = false;
        for (size_t marker_index = 0u;
             marker_index < sizeof(unsupported_markers) / sizeof(unsupported_markers[0]);
             ++marker_index) {
            if (parameter == p_find_symbol(p, p->cl,
                                           unsupported_markers[marker_index], NULL)) {
                unsupported = true;
                break;
            }
        }
        if (unsupported) break;
        if (p_type(parameter) != P_SYMBOL) break;
        if ((p_symbol_ptr(parameter)->flags & P_CONSTANT) != 0u) {
            (void)p_error(p, P_ERR_CONTROL, "constant symbol cannot be a parameter");
            p->error.datum = parameter;
            return 0u;
        }
        ++count;
        cursor = p_cons_ptr(cursor)->cdr;
    }
    if (cursor != p->nil || (saw_rest && p_type(rest) != P_SYMBOL)) {
        (void)p_error(p, P_ERR_UNSUPPORTED,
                      "stage-0 lambda lists require parameters and optional &rest only");
        return 0u;
    }
    PClosure *closure = (PClosure *)p_alloc(p, P_CLOSURE, sizeof(*closure));
    if (closure == NULL) return 0u;
    closure->name = name;
    closure->parameters = parameters;
    closure->body = body;
    closure->environment = environment;
    closure->required_count = count;
    closure->rest_parameter = rest;
    closure->flags = macro ? P_EVAL_MACRO : 0u;
    return (Value)closure;
}

static size_t control_token(PState *state) {
    ++state->next_control_token;
    if (state->next_control_token == 0u ||
        state->next_control_token > (uint64_t)p_fixnum_max())
        state->next_control_token = 1u;
    return (size_t)state->next_control_token;
}

static bool arity_error(PState *state, size_t want_min, size_t want_max,
                        size_t got) {
    char message[128];
    (void)snprintf(message, sizeof(message),
                   "wrong number of arguments (expected %lu..%s, got %lu)",
                   (unsigned long)want_min,
                   want_max == P_EVAL_PRIMITIVE_VARIADIC ? "many" : "bounded",
                   (unsigned long)got);
    (void)p_error(state->p, P_ERR_ARITY, message);
    return false;
}

static bool closure_arguments(PState *state, PClosure *closure,
                              size_t argc, const Value *argv,
                              Value *environment_out, bool *dynamic_out) {
    Pocket *p = state->p;
    if (argc < closure->required_count ||
        (closure->rest_parameter == 0u && argc != closure->required_count)) {
        (void)arity_error(state, closure->required_count,
                          closure->rest_parameter == 0u ? closure->required_count : P_EVAL_PRIMITIVE_VARIADIC,
                          argc);
        state->p->error.datum = closure->name != state->p->nil ?
                                closure->name : closure->parameters;
        state->p->error.expected = p_fixnum((int64_t)argc);
        return false;
    }
    Value roots[4] = { (Value)closure, closure->environment, 0u, 0u };
    size_t saved = p_root_save(p);
    size_t rest_count = argc > closure->required_count ? argc - closure->required_count : 0u;
    size_t binding_count = closure->required_count +
                           (closure->rest_parameter != 0u ? 1u : 0u);
    if (binding_count > (SIZE_MAX - offsetof(PRecord, data) - ENV_SLOTS * sizeof(Value)) /
                        (2u * sizeof(PCons)) ||
        rest_count > (SIZE_MAX - offsetof(PRecord, data) - ENV_SLOTS * sizeof(Value) -
                      binding_count * 2u * sizeof(PCons)) / sizeof(PCons)) {
        (void)p_error(p, P_ERR_RANGE, "closure argument environment is too large");
        return false;
    }
    size_t reserve_bytes = offsetof(PRecord, data) + ENV_SLOTS * sizeof(Value) +
                           binding_count * 2u * sizeof(PCons) +
                           rest_count * sizeof(PCons);
    if (!p_roots(p, roots, 4u) || !p_reserve(p, reserve_bytes)) {
        p_root_restore(p, saved);
        return false;
    }
    roots[2] = env_create_reserved(state, roots[1]);
    if (roots[2] == 0u) { p_root_restore(p, saved); return false; }
    bool has_dynamic_parameters = false;
    size_t i = 0u;
    Value cursor = ((PClosure *)roots[0])->parameters;
    for (; i < ((PClosure *)roots[0])->required_count; ++i) {
        Value symbol = p_cons_ptr(cursor)->car;
        PClosure *current = (PClosure *)roots[0];
        bool special = (p_symbol_ptr(symbol)->flags & P_SPECIAL) != 0u ||
                       is_declared_special(p, current->body, symbol);
        bool bound;
        if (special) {
            Value old = p_symbol_ptr(symbol)->value;
            if (!dynamic_push(state, DYNAMIC_SPECIAL, symbol, old, 0u, 0u)) {
                p_root_restore(p, saved); return false;
            }
            p_symbol_ptr(symbol)->value = argv[i];
            bound = env_bind_reserved(state, roots[2], P_ENV_VALUE, symbol, P_UNBOUND);
            has_dynamic_parameters = true;
        } else bound = env_bind_reserved(state, roots[2], P_ENV_VALUE, symbol, argv[i]);
        if (!bound) {
            p_root_restore(p, saved); return false;
        }
        cursor = p_cons_ptr(cursor)->cdr;
    }
    PClosure *current = (PClosure *)roots[0];
    if (current->rest_parameter != 0u) {
        roots[3] = p->nil;
        for (size_t j = argc; j > i; --j) {
            roots[3] = p_cons(p, argv[j - 1u], roots[3]);
            if (roots[3] == 0u) { p_root_restore(p, saved); return false; }
        }
        Value symbol = current->rest_parameter;
        bool special = (p_symbol_ptr(symbol)->flags & P_SPECIAL) != 0u ||
                       is_declared_special(p, current->body, symbol);
        bool bound;
        if (special) {
            Value old = p_symbol_ptr(symbol)->value;
            if (!dynamic_push(state, DYNAMIC_SPECIAL, symbol, old, 0u, 0u)) {
                p_root_restore(p, saved); return false;
            }
            p_symbol_ptr(symbol)->value = roots[3];
            bound = env_bind_reserved(state, roots[2], P_ENV_VALUE, symbol, P_UNBOUND);
            has_dynamic_parameters = true;
        } else bound = env_bind_reserved(state, roots[2], P_ENV_VALUE, symbol, roots[3]);
        if (!bound) { p_root_restore(p, saved); return false; }
    }
    /* Declarations also make free references in this function dynamic, even
     * when the declared name is inherited from an enclosing lexical scope. */
    for (Value form = ((PClosure *)roots[0])->body; p_type(form) == P_CONS;
         form = p_cons_ptr(form)->cdr) {
        Value declaration = p_cons_ptr(form)->car;
        if (p_type(declaration) != P_CONS ||
            !symbol_named(p, p_cons_ptr(declaration)->car, "DECLARE")) break;
        for (Value spec = p_cons_ptr(declaration)->cdr; p_type(spec) == P_CONS;
             spec = p_cons_ptr(spec)->cdr) {
            Value entry = p_cons_ptr(spec)->car;
            if (p_type(entry) != P_CONS ||
                !symbol_named(p, p_cons_ptr(entry)->car, "SPECIAL")) continue;
            for (Value names = p_cons_ptr(entry)->cdr; p_type(names) == P_CONS;
                 names = p_cons_ptr(names)->cdr) {
                Value name = p_cons_ptr(names)->car;
                if (find_local_binding(roots[2], P_ENV_VALUE, name) == 0u &&
                    !env_bind_special_marker(state, roots[2], name)) {
                    p_root_restore(p, saved); return false;
                }
            }
        }
    }
    *environment_out = roots[2];
    *dynamic_out = has_dynamic_parameters;
    p_root_restore(p, saved);
    return true;
}

static Value apply_values(PState *state, Value function, size_t argc,
                          const Value *argv) {
    Pocket *p = state->p;
    Value trampoline_args[512] = { 0u };
    Value function_root = function;
    size_t saved = p_root_save(p);
    if (!p_roots(p, &function_root, 1u) || !p_roots(p, trampoline_args, 512u)) {
        p_root_restore(p, saved);
        return 0u;
    }
    for (;;) {
        function = function_root;
        if (p_type(function) == P_PRIMITIVE) {
            PPrimitive *primitive = (PPrimitive *)function;
            if (argc < primitive->min_arity || argc > primitive->max_arity) {
                (void)arity_error(state, primitive->min_arity, primitive->max_arity, argc);
                p->error.datum = primitive->name;
                p->error.expected = p_fixnum((int64_t)argc);
                p_root_restore(p, saved);
                return 0u;
            }
            if ((primitive->flags & P_PRIMITIVE_EVAL_FUNCALL) != 0u) {
                if (argc == 0u) {
                    (void)arity_error(state, 1u, P_EVAL_PRIMITIVE_VARIADIC, 0u);
                    p_root_restore(p, saved);
                    return 0u;
                }
                Value target = global_function_designator(state, argv[0]);
                Value result = p->error.code == P_OK ?
                    apply_values(state, target, argc - 1u, &argv[1]) : 0u;
                p_root_restore(p, saved);
                return result;
            }
            if ((primitive->flags & P_PRIMITIVE_EVAL_APPLY) != 0u) {
                if (argc < 2u) {
                    (void)arity_error(state, 2u, P_EVAL_PRIMITIVE_VARIADIC, argc);
                    p_root_restore(p, saved);
                    return 0u;
                }
                Value arguments[ARGUMENT_LIMIT] = { 0u };
                size_t arg_saved = p_root_save(p);
                if (!p_roots(p, arguments, ARGUMENT_LIMIT)) {
                    p_root_restore(p, arg_saved); p_root_restore(p, saved); return 0u;
                }
                size_t n = 0u;
                for (size_t i = 1u; i + 1u < argc && n < ARGUMENT_LIMIT; ++i)
                    arguments[n++] = argv[i];
                Value tail = argv[argc - 1u];
                while (p_type(tail) == P_CONS && n < ARGUMENT_LIMIT) {
                    arguments[n++] = p_cons_ptr(tail)->car;
                    tail = p_cons_ptr(tail)->cdr;
                }
                if (tail != p->nil)
                    (void)p_error(p, P_ERR_TYPE, "APPLY final argument must be a proper list");
                Value target = p->error.code == P_OK ?
                    global_function_designator(state, argv[0]) : 0u;
                Value result = p->error.code == P_OK ?
                    apply_values(state, target, n, arguments) : 0u;
                p_root_restore(p, arg_saved);
                p_root_restore(p, saved);
                return result;
            }
            size_t bytes = 0u;
            if (primitive->reserve != NULL &&
                !primitive->reserve(state, argc, argv, &bytes)) {
                p_root_restore(p, saved);
                return 0u;
            }
            if (bytes != 0u && !p_reserve(p, bytes)) {
                p_root_restore(p, saved);
                return 0u;
            }
            state->mv_count = 0u;
            Value result = primitive->entry(state, argc, argv);
            if (result == 0u && p->error.code == P_OK)
                (void)p_error(p, P_ERR_UNSUPPORTED, "primitive failed without an error");
            if (p->error.code != P_OK) {
                p_root_restore(p, saved);
                return 0u;
            }
            if (state->mv_count == 0u &&
                (primitive->flags & P_PRIMITIVE_EVAL_VALUES) == 0u) {
                state->mv[0] = result;
                state->mv_count = 1u;
            }
            p_root_restore(p, saved);
            return result;
        }
        if (p_type(function) != P_CLOSURE) {
            (void)p_error(p, P_ERR_TYPE, "attempt to call a non-function");
            p->error.datum = function;
            p_root_restore(p, saved);
            return 0u;
        }
        if (state->call_depth >= state->max_call_depth) {
            (void)p_error(p, P_ERR_STACK, "stage-0 call depth exceeded");
            p_root_restore(p, saved);
            return 0u;
        }
        Value roots[5] = { function_root, 0u, state->current_env, 0u, 0u };
        volatile size_t local_saved = p_root_save(p);
        size_t dynamic_base = internals(state)->dynamic_count;
        bool dynamic_parameters = false;
        if (!p_roots(p, roots, 5u)) {
            p_root_restore(p, local_saved);
            p_root_restore(p, saved);
            return 0u;
        }
        if (!closure_arguments(state, (PClosure *)roots[0], argc, argv, &roots[1],
                               &dynamic_parameters)) {
            (void)unwind_dynamic(state, dynamic_base);
            p_root_restore(p, local_saved);
            p_root_restore(p, saved);
            return 0u;
        }
        ++state->call_depth;
        state->tail_pending = false;
        state->current_env = roots[1];
        Value closure_name = ((PClosure *)roots[0])->name;
        ControlFrame * volatile named_block = NULL;
        volatile bool block_tail = !dynamic_parameters;
        /* The conservative syntax scan is also an allocation proof: if there
         * is no RETURN-FROM to this name, and no closure/macro form that could
         * hide one, the implicit BLOCK has no observable target. Avoiding its
         * environment and control frame keeps ordinary named functions cheap. */
        bool named_block_needed = closure_name != p->nil &&
            block_tail_unsafe(state, ((PClosure *)roots[0])->body,
                              closure_name, roots[1]);
        if (named_block_needed) {
            roots[3] = p_fixnum((int64_t)control_token(state));
            roots[4] = p_env_create(state, roots[1]);
            if (roots[4] != 0u &&
                p_env_bind(state, roots[4], P_ENV_BLOCK, closure_name,
                           roots[3], false)) {
                roots[1] = roots[4];
                state->current_env = roots[1];
                named_block = control_push(state, CONTROL_BLOCK, roots[3]);
                block_tail = false;
            }
        }
        Value result = p->error.code == P_OK &&
                       (!named_block_needed || named_block != NULL) ?
            (named_block == NULL ?
                eval_sequence(state, ((PClosure *)roots[0])->body, roots[1],
                              block_tail) :
                p->nil) : 0u;
        if (named_block != NULL) {
            int jumped = setjmp(named_block->jump);
            if (jumped == 0) {
                result = eval_sequence(state, ((PClosure *)roots[0])->body,
                                       roots[1], block_tail);
            } else {
                recover_control_jump(state, named_block);
                (void)unwind_dynamic(state, named_block->dynamic_base);
                restore_transfer_values(state, named_block);
                result = state->mv_count == 0u ? p->nil : state->mv[0];
            }
            control_pop(state, named_block);
        }
        (void)unwind_dynamic(state, dynamic_base);
        --state->call_depth;
        state->current_env = roots[2];
        p_root_restore(p, local_saved);
        if (p->error.code != P_OK) {
            p_root_restore(p, saved);
            return 0u;
        }
        if (!state->tail_pending) {
            p_root_restore(p, saved);
            return result;
        }
        if (state->tail_argc > 512u) {
            (void)p_error(p, P_ERR_STACK, "too many tail-call arguments");
            p_root_restore(p, saved);
            return 0u;
        }
        function_root = state->tail_function;
        argc = state->tail_argc;
        for (size_t i = 0u; i < argc; ++i)
            trampoline_args[i] = state->tail_arguments[i];
        argv = trampoline_args;
        state->tail_pending = false;
    }
}

Value p_apply0(PState *state, Value function, Value arguments) {
    Value argv[ARGUMENT_LIMIT] = { 0u };
    Value fn_root = function;
    size_t argc = 0u, saved = p_root_save(state->p);
    if (!p_roots(state->p, &fn_root, 1u) ||
        !p_roots(state->p, argv, ARGUMENT_LIMIT)) {
        p_root_restore(state->p, saved);
        return 0u;
    }
    while (p_type(arguments) == P_CONS) {
        if (argc == ARGUMENT_LIMIT) {
            (void)p_error(state->p, P_ERR_STACK, "too many stage-0 arguments");
            p_root_restore(state->p, saved);
            return 0u;
        }
        argv[argc++] = p_cons_ptr(arguments)->car;
        arguments = p_cons_ptr(arguments)->cdr;
    }
    if (arguments != state->p->nil) {
        (void)p_error(state->p, P_ERR_TYPE, "apply arguments must be a proper list");
        p_root_restore(state->p, saved);
        return 0u;
    }
    Value result = apply_values(state, fn_root, argc, argv);
    p_root_restore(state->p, saved);
    return result;
}

static Value eval_arguments(PState *state, Value arguments, Value environment,
                            Value *argv, size_t *argc_out) {
    size_t argc = 0u;
    while (p_type(arguments) == P_CONS) {
        if (argc == ARGUMENT_LIMIT) {
            (void)p_error(state->p, P_ERR_STACK, "too many stage-0 arguments");
            return 0u;
        }
        argv[argc] = eval_form(state, p_cons_ptr(arguments)->car, environment, false);
        if (state->p->error.code != P_OK) return 0u;
        ++argc;
        arguments = p_cons_ptr(arguments)->cdr;
    }
    if (arguments != state->p->nil) {
        (void)p_error(state->p, P_ERR_TYPE, "function call has an improper argument list");
        return 0u;
    }
    *argc_out = argc;
    return state->p->nil;
}

static Value eval_symbol(PState *state, Value symbol, Value environment) {
    Pocket *p = state->p;
    Value binding = find_binding(environment, P_ENV_VALUE, symbol);
    if (binding != 0u && p_cons_ptr(binding)->cdr != P_UNBOUND)
        return p_cons_ptr(binding)->cdr;
    if (p_type(symbol) != P_SYMBOL) return 0u;
    /* A symbol has a value cell even when nobody proclaimed it SPECIAL.  A
     * SET at top level (or a free reference) uses that cell; SPECIAL only
     * controls whether lexical binding forms also update the dynamic cell. */
    Value value = p_symbol_ptr(symbol)->value;
    if (value != P_UNBOUND) return value;
    (void)p_error(p, P_ERR_UNBOUND, "unbound variable");
    p->error.datum = symbol;
    return 0u;
}

static Value function_value(PState *state, Value designator, Value environment) {
    Pocket *p = state->p;
    if (p_type(designator) == P_SYMBOL) {
        Value value = p_env_lookup(state, environment, P_ENV_FUNCTION, designator);
        if (value != 0u) return value;
        value = p_symbol_ptr(designator)->function;
        if (value == P_UNBOUND) {
            (void)p_error(p, P_ERR_UNDEFINED, "undefined function");
            p->error.datum = designator;
            return 0u;
        }
        return value;
    }
    return eval_form(state, designator, environment, false);
}

/* FUNCALL and APPLY accept symbols as function designators.  Their contract
 * is deliberately the symbol's global function cell: FLET/LABELS bindings
 * are resolved only for operator syntax, never for a quoted designator. */
static Value global_function_designator(PState *state, Value designator) {
    if (p_type(designator) != P_SYMBOL) return designator;
    Value function = p_symbol_ptr(designator)->function;
    if (function == P_UNBOUND) {
        (void)p_error(state->p, P_ERR_UNDEFINED, "undefined function designator");
        state->p->error.datum = designator;
        return 0u;
    }
    return function;
}

static Value eval_sequence(PState *state, Value body, Value environment,
                          bool tail_position) {
    while (p_type(body) == P_CONS &&
           p_type(p_cons_ptr(body)->car) == P_CONS &&
           symbol_named(state->p, p_cons_ptr(p_cons_ptr(body)->car)->car, "DECLARE"))
        body = p_cons_ptr(body)->cdr;
    if (body == state->p->nil) {
        state->mv[0] = state->p->nil; state->mv_count = 1u;
        return state->p->nil;
    }
    while (p_type(body) == P_CONS) {
        Value next = p_cons_ptr(body)->cdr;
        if (next == state->p->nil) return eval_form(state, p_cons_ptr(body)->car, environment, tail_position);
        if (p_type(next) != P_CONS) {
            (void)p_error(state->p, P_ERR_TYPE, "body must be a proper list");
            return 0u;
        }
        (void)eval_form(state, p_cons_ptr(body)->car, environment, false);
        if (state->p->error.code != P_OK) return 0u;
        body = next;
    }
    (void)tail_position;
    (void)p_error(state->p, P_ERR_TYPE, "body must be a proper list");
    return 0u;
}

static bool is_declared_special(Pocket *p, Value body, Value symbol) {
    for (Value forms = body; p_type(forms) == P_CONS; forms = p_cons_ptr(forms)->cdr) {
        Value form = p_cons_ptr(forms)->car;
        if (p_type(form) != P_CONS ||
            !symbol_named(p, p_cons_ptr(form)->car, "DECLARE")) break;
        for (Value specs = p_cons_ptr(form)->cdr; p_type(specs) == P_CONS;
             specs = p_cons_ptr(specs)->cdr) {
            Value spec = p_cons_ptr(specs)->car;
            if (p_type(spec) != P_CONS ||
                !symbol_named(p, p_cons_ptr(spec)->car, "SPECIAL")) continue;
            for (Value names = p_cons_ptr(spec)->cdr; p_type(names) == P_CONS;
                 names = p_cons_ptr(names)->cdr)
                if (p_cons_ptr(names)->car == symbol) return true;
        }
    }
    return false;
}

/* Install lexical markers for SPECIAL declarations so an enclosing lexical
 * value binding cannot capture references that must use the dynamic cell. */
static bool bind_declared_special_markers(PState *state, Value environment,
                                          Value body) {
    Pocket *p = state->p;
    for (Value form = body; p_type(form) == P_CONS; form = p_cons_ptr(form)->cdr) {
        Value declaration = p_cons_ptr(form)->car;
        if (p_type(declaration) != P_CONS ||
            !symbol_named(p, p_cons_ptr(declaration)->car, "DECLARE")) break;
        for (Value spec = p_cons_ptr(declaration)->cdr; p_type(spec) == P_CONS;
             spec = p_cons_ptr(spec)->cdr) {
            Value entry = p_cons_ptr(spec)->car;
            if (p_type(entry) != P_CONS ||
                !symbol_named(p, p_cons_ptr(entry)->car, "SPECIAL")) continue;
            for (Value names = p_cons_ptr(entry)->cdr; p_type(names) == P_CONS;
                 names = p_cons_ptr(names)->cdr) {
                Value name = p_cons_ptr(names)->car;
                if (find_local_binding(environment, P_ENV_VALUE, name) == 0u &&
                    !env_bind_special_marker(state, environment, name)) return false;
            }
        }
    }
    return state->p->error.code == P_OK;
}

/* A block without a matching RETURN-FROM can pass through a tail call only
 * when its body cannot create a closure or expand a macro into a nonlocal
 * exit. Be conservative around those forms so captured block tokens remain
 * live for deferred calls. */
static bool block_tail_unsafe(PState *state, Value tree, Value name,
                              Value environment) {
    Pocket *p = state->p;
    size_t budget = p->heap_size / P_MIN_BLOCK;
    Value pending[ARGUMENT_LIMIT];
    size_t count = 0u;
    pending[count++] = tree;
    while (count != 0u && budget-- != 0u) {
        Value value = pending[--count];
        if (p_type(value) != P_CONS) continue;
        Value head = p_cons_ptr(value)->car;
        Value tail = p_cons_ptr(value)->cdr;
        if (symbol_named(p, head, "RETURN-FROM") &&
            p_type(tail) == P_CONS && p_cons_ptr(tail)->car == name)
            return true;
        if (symbol_named(p, head, "LAMBDA") ||
            (symbol_named(p, head, "FUNCTION") && p_type(tail) == P_CONS &&
             symbol_named(p, p_cons_ptr(tail)->car, "LAMBDA")) ||
            symbol_named(p, head, "FLET") || symbol_named(p, head, "LABELS") ||
            symbol_named(p, head, "MACROLET") || symbol_named(p, head, "SYMBOL-MACROLET"))
            return true;
        if (p_type(head) == P_SYMBOL &&
            (((p_symbol_ptr(head)->flags & P_MACRO) != 0u) ||
             p_env_lookup(state, environment, P_ENV_MACRO, head) != 0u))
            return true;
        if (count + 2u > ARGUMENT_LIMIT) return true;
        pending[count++] = head;
        pending[count++] = tail;
    }
    return count != 0u;
}

static void request_tail_call(PState *state, Value function, size_t argc,
                              const Value *argv) {
    if (argc > 512u) {
        (void)p_error(state->p, P_ERR_STACK, "too many tail-call arguments");
        return;
    }
    state->tail_function = function;
    state->tail_argc = argc;
    for (size_t i = 0u; i < argc; ++i) state->tail_arguments[i] = argv[i];
    state->tail_pending = true;
}

static Value expand_lambda_list(PState *state, Value parameters, Value body,
                                bool macro, Value environment) {
    Value closure = 0u;
    if (!macro) {
        closure = p_make_closure(state, parameters, body, environment,
                                 false, state->p->nil);
        if (state->p->error.code == P_OK) return closure;
        if (state->p->error.code != P_ERR_UNSUPPORTED) return 0u;
        p_clear_error(state->p);
    }
    int status = 0;
    Value expander = p_find_symbol(state->p, state->p->sys,
                                   macro ? "%EXPAND-MACRO-LAMBDA" : "%EXPAND-LAMBDA",
                                   &status);
    if (status == 0 || p_type(expander) != P_SYMBOL ||
        p_symbol_ptr(expander)->function == P_UNBOUND) {
        if (macro)
            return p_make_closure(state, parameters, body, environment, true,
                                  state->p->nil);
        (void)p_error(state->p, P_ERR_UNSUPPORTED,
                      "rich lambda lists require the boot lambda-list expander");
        return 0u;
    }
    Value roots[7] = { parameters, body, environment,
                       p_symbol_ptr(expander)->function, 0u, 0u, 0u };
    size_t saved = p_root_save(state->p);
    if (!p_roots(state->p, roots, 7u) || !p_reserve(state->p, 4u * sizeof(PCons))) {
        p_root_restore(state->p, saved); return 0u;
    }
    Value args = p_cons(state->p, roots[1], state->p->nil);
    if (args == 0u) { p_root_restore(state->p, saved); return 0u; }
    Value args_root = args;
    size_t args_saved = p_root_save(state->p);
    if (!p_roots(state->p, &args_root, 1u)) { p_root_restore(state->p, saved); return 0u; }
    args = p_cons(state->p, roots[0], args_root);
    if (args == 0u) { p_root_restore(state->p, args_saved); p_root_restore(state->p, saved); return 0u; }
    roots[4] = p_apply0(state, roots[3], args);
    p_root_restore(state->p, args_saved);
    if (roots[4] == 0u || p_type(roots[4]) != P_CONS ||
        !symbol_named(state->p, p_cons_ptr(roots[4])->car, "LAMBDA")) {
        p_root_restore(state->p, saved);
        if (state->p->error.code == P_OK)
            (void)p_error(state->p, P_ERR_TYPE, "lambda-list expander must return a LAMBDA form");
        return 0u;
    }
    Value tail = p_cons_ptr(roots[4])->cdr;
    roots[5] = p_type(tail) == P_CONS ? p_cons_ptr(tail)->car : state->p->nil;
    roots[6] = p_type(tail) == P_CONS ? p_cons_ptr(tail)->cdr : state->p->nil;
    Value result = p_reserve(state->p, sizeof(PClosure)) ?
        p_make_closure(state, roots[5], roots[6], roots[2], macro,
                       state->p->nil) : 0u;
    p_root_restore(state->p, saved);
    return result;
}

static Value make_closure_at_safe_point(PState *state, Value params, Value body,
                                        Value environment, bool macro, Value name) {
    Value roots[4] = { params, body, environment, name };
    size_t saved = p_root_save(state->p);
    if (!p_roots(state->p, roots, 4u) || !p_reserve(state->p, sizeof(PClosure))) {
        p_root_restore(state->p, saved); return 0u;
    }
    Value closure = expand_lambda_list(state, roots[0], roots[1], macro, roots[2]);
    if (closure != 0u && p_type(closure) == P_CLOSURE)
        ((PClosure *)closure)->name = roots[3];
    p_root_restore(state->p, saved);
    return closure;
}

static Value eval_form(PState *state, Value form, Value environment, bool tail_position) {
    Pocket *p = state->p;
    Value roots[16] = { form, environment, 0u };
    size_t root_saved = p_root_save(p);
    if (state->call_depth >= state->max_call_depth) {
        (void)p_error(p, P_ERR_STACK, "stage-0 evaluation depth exceeded");
        return 0u;
    }
    if (!p_roots(p, roots, 16u)) return 0u;
    roots[14] = tail_position ? p->truth : p->nil;
    roots[15] = state->current_env;
    state->current_env = roots[1];
    ++state->call_depth;
    Value result = p->nil;
    for (;;) {
        form = roots[0];
        if (p->error.code != P_OK) { result = 0u; break; }
        if (p_type(form) == P_SYMBOL) {
            Value expansion = lookup_symbol_macro(state, roots[1], form);
            if (expansion != 0u) { roots[0] = expansion; continue; }
            result = eval_symbol(state, form, roots[1]);
            state->mv[0] = result; state->mv_count = 1u;
            break;
        }
        if (p_type(form) != P_CONS) {
            result = form;
            state->mv[0] = result; state->mv_count = 1u;
            break;
        }
        roots[2] = p_cons_ptr(form)->car;
        roots[3] = p_cons_ptr(form)->cdr;
        if (symbol_named(p, roots[2], "QUOTE")) {
            result = list_nth(p, roots[3], 0u);
            state->mv[0] = result; state->mv_count = 1u; break;
        }
        if (symbol_named(p, roots[2], "IF")) {
            size_t count = 0u;
            if (!proper_list(p, roots[3], &count) || count < 2u) {
                (void)p_error(p, P_ERR_ARITY, "IF requires a test and branch"); break;
            }
            roots[4] = list_nth(p, roots[3], 0u);
            roots[5] = eval_form(state, roots[4], roots[1], false);
            if (p->error.code != P_OK) break;
            roots[6] = list_nth(p, roots[3], 1u);
            roots[7] = count > 2u ? list_nth(p, roots[3], 2u) : p->nil;
            if (roots[5] == p->nil) roots[0] = roots[7];
            else roots[0] = roots[6];
            continue;
        }
        if (symbol_named(p, roots[2], "PROGN")) {
            if (roots[3] == p->nil) {
                result = p->nil; state->mv[0] = result; state->mv_count = 1u; break;
            }
            result = eval_sequence(state, roots[3], roots[1], (roots[14] != p->nil));
            break;
        }
        if (symbol_named(p, roots[2], "BLOCK")) {
            if (p_type(roots[3]) != P_CONS) {
                (void)p_error(p, P_ERR_ARITY, "BLOCK requires a name"); break;
            }
            roots[4] = p_cons_ptr(roots[3])->car;
            Value block_body = p_cons_ptr(roots[3])->cdr;
            /* When the conservative scanner finds no possible lexical exit,
             * the BLOCK has no observable binding or dynamic extent. Keep the
             * caller's tail position and skip its temporary environment. */
            if (!block_tail_unsafe(state, block_body, roots[4], roots[1])) {
                result = eval_sequence(state, block_body, roots[1],
                                       roots[14] != p->nil);
                break;
            }
            roots[5] = p_fixnum((int64_t)control_token(state));
            roots[6] = p_env_create(state, roots[1]);
            if (roots[6] == 0u ||
                !p_env_bind(state, roots[6], P_ENV_BLOCK, roots[4], roots[5], false)) break;
            ControlFrame *frame = control_push(state, CONTROL_BLOCK, roots[5]);
            if (frame == NULL) break;
            bool block_tail = false;
            int jumped = setjmp(frame->jump);
            if (jumped == 0) {
                result = eval_sequence(state, block_body, roots[6], block_tail);
            } else {
                recover_control_jump(state, frame);
                (void)unwind_dynamic(state, frame->dynamic_base);
                restore_transfer_values(state, frame);
                result = state->mv_count == 0u ? p->nil : state->mv[0];
            }
            control_pop(state, frame);
            break;
        }
        if (symbol_named(p, roots[2], "RETURN-FROM")) {
            if (p_type(roots[3]) != P_CONS) {
                (void)p_error(p, P_ERR_ARITY, "RETURN-FROM requires a name"); break;
            }
            roots[4] = p_cons_ptr(roots[3])->car;
            roots[5] = p_env_lookup(state, roots[1], P_ENV_BLOCK, roots[4]);
            if (roots[5] == 0u) {
                (void)p_error(p, P_ERR_UNBOUND, "no lexically visible BLOCK"); break;
            }
            if (p_type(p_cons_ptr(roots[3])->cdr) == P_CONS) {
                roots[6] = eval_form(state, p_cons_ptr(p_cons_ptr(roots[3])->cdr)->car,
                                     roots[1], false);
            } else {
                roots[6] = p->nil;
                state->mv[0] = p->nil; state->mv_count = 1u;
            }
            if (p->error.code != P_OK) break;
            ControlFrame *frame = find_control(state, CONTROL_BLOCK, roots[5]);
            if (frame == NULL) {
                (void)p_error(p, P_ERR_UNBOUND, "BLOCK target is no longer active"); break;
            }
            if (!save_transfer_values(state, frame)) break;
            (void)unwind_dynamic(state, frame->dynamic_base);
            longjmp(frame->jump, 1);
        }
        if (symbol_named(p, roots[2], "TAGBODY")) {
            roots[4] = p_env_create(state, roots[1]);
            if (roots[4] == 0u) break;
            roots[5] = p_fixnum((int64_t)control_token(state));
            size_t count = 0u;
            if (!proper_list(p, roots[3], &count) || count > 256u) {
                (void)p_error(p, P_ERR_STACK, "TAGBODY is too large or improper"); break;
            }
            size_t index = 0u;
            for (Value cursor = roots[3]; p_type(cursor) == P_CONS;
                 cursor = p_cons_ptr(cursor)->cdr, ++index) {
                Value item = p_cons_ptr(cursor)->car;
                if (p_type(item) == P_CONS) continue;
                Value target = p_cons(p, roots[5], p_fixnum((int64_t)index));
                if (target == 0u || !p_env_bind(state, roots[4], P_ENV_TAG,
                                                item, target, false)) break;
            }
            if (p->error.code != P_OK) break;
            ControlFrame *frame = control_push(state, CONTROL_TAGBODY, roots[5]);
            if (frame == NULL) break;
            volatile size_t next_index = 0u;
            int jumped = setjmp(frame->jump);
            if (jumped != 0) {
                recover_control_jump(state, frame);
                next_index = frame->target_index + 1u;
            }
            while (next_index < count && p->error.code == P_OK) {
                Value cursor = roots[3];
                for (size_t j = 0u; j < next_index; ++j) cursor = p_cons_ptr(cursor)->cdr;
                Value item = p_cons_ptr(cursor)->car;
                if (p_type(item) == P_CONS) (void)eval_form(state, item, roots[4], false);
                ++next_index;
            }
            if (p->error.code == P_OK) { result = p->nil; state->mv[0] = result; state->mv_count = 1u; }
            control_pop(state, frame);
            break;
        }
        if (symbol_named(p, roots[2], "GO")) {
            if (p_type(roots[3]) != P_CONS) { (void)p_error(p, P_ERR_ARITY, "GO requires a tag"); break; }
            roots[4] = p_cons_ptr(roots[3])->car;
            roots[5] = p_env_lookup(state, roots[1], P_ENV_TAG, roots[4]);
            if (p_type(roots[5]) != P_CONS) {
                (void)p_error(p, P_ERR_UNBOUND, "no lexically visible TAGBODY tag"); break;
            }
            ControlFrame *frame = find_control(state, CONTROL_TAGBODY,
                                               p_cons_ptr(roots[5])->car);
            if (frame == NULL) { (void)p_error(p, P_ERR_UNBOUND, "TAGBODY target is no longer active"); break; }
            Value target_index = p_cons_ptr(roots[5])->cdr;
            if (!p_is_fixnum(target_index) || p_fixnum_value(target_index) < 0) {
                (void)p_error(p, P_ERR_CONTROL, "invalid TAGBODY target token"); break;
            }
            frame->target_index = (size_t)p_fixnum_value(target_index);
            (void)unwind_dynamic(state, frame->dynamic_base);
            longjmp(frame->jump, 1);
        }
        if (symbol_named(p, roots[2], "CATCH")) {
            if (p_type(roots[3]) != P_CONS) { (void)p_error(p, P_ERR_ARITY, "CATCH requires a tag"); break; }
            roots[4] = eval_form(state, p_cons_ptr(roots[3])->car, roots[1], false);
            if (p->error.code != P_OK) break;
            ControlFrame *frame = control_push(state, CONTROL_CATCH, roots[4]);
            if (frame == NULL) break;
            int jumped = setjmp(frame->jump);
            if (jumped == 0)
                result = eval_sequence(state, p_cons_ptr(roots[3])->cdr, roots[1], false);
            else {
                recover_control_jump(state, frame);
                (void)unwind_dynamic(state, frame->dynamic_base);
                restore_transfer_values(state, frame);
                result = state->mv_count == 0u ? p->nil : state->mv[0];
            }
            control_pop(state, frame);
            break;
        }
        if (symbol_named(p, roots[2], "THROW")) {
            if (!proper_list(p, roots[3], NULL) ||
                p_type(roots[3]) != P_CONS || p_type(p_cons_ptr(roots[3])->cdr) != P_CONS) {
                (void)p_error(p, P_ERR_ARITY, "THROW requires a tag and value"); break;
            }
            roots[4] = eval_form(state, p_cons_ptr(roots[3])->car, roots[1], false);
            roots[5] = eval_form(state, p_cons_ptr(p_cons_ptr(roots[3])->cdr)->car,
                                 roots[1], false);
            if (p->error.code != P_OK) break;
            ControlFrame *frame = find_control(state, CONTROL_CATCH, roots[4]);
            if (frame == NULL) { (void)p_error(p, P_ERR_UNBOUND, "no matching CATCH"); break; }
            if (!save_transfer_values(state, frame)) break;
            (void)unwind_dynamic(state, frame->dynamic_base);
            longjmp(frame->jump, 1);
        }
        if (symbol_named(p, roots[2], "UNWIND-PROTECT")) {
            Value forms = p_cons_ptr(roots[3])->cdr;
            size_t dynamic_base = internals(state)->dynamic_count;
            if (!dynamic_push(state, DYNAMIC_CLEANUP, 0u, 0u, forms, roots[1])) break;
            result = eval_form(state, p_cons_ptr(roots[3])->car, roots[1], false);
            if (p->error.code == P_OK) {
                Value held[P_MULTIPLE_VALUES_LIMIT] = { 0u };
                size_t held_count = state->mv_count;
                if (held_count > P_MULTIPLE_VALUES_LIMIT) held_count = P_MULTIPLE_VALUES_LIMIT;
                size_t held_root = p_root_save(p);
                if (!p_roots(p, held, P_MULTIPLE_VALUES_LIMIT)) { p_root_restore(p, held_root); break; }
                memcpy(held, state->mv, held_count * sizeof(Value));
                (void)unwind_dynamic(state, dynamic_base);
                if (p->error.code == P_OK) {
                    state->mv_count = held_count;
                    memcpy(state->mv, held, held_count * sizeof(Value));
                }
                p_root_restore(p, held_root);
            }
            break;
        }
        if (symbol_named(p, roots[2], "MULTIPLE-VALUE-PROG1")) {
            if (roots[3] == p->nil) { (void)p_error(p, P_ERR_ARITY, "MULTIPLE-VALUE-PROG1 requires a form"); break; }
            roots[4] = p_cons_ptr(roots[3])->car;
            result = eval_form(state, roots[4], roots[1], false);
            if (p->error.code != P_OK) break;
            Value held[P_MULTIPLE_VALUES_LIMIT] = { 0u };
            size_t held_count = state->mv_count;
            if (held_count > P_MULTIPLE_VALUES_LIMIT) held_count = P_MULTIPLE_VALUES_LIMIT;
            size_t held_root = p_root_save(p);
            if (!p_roots(p, held, P_MULTIPLE_VALUES_LIMIT)) { p_root_restore(p, held_root); break; }
            memcpy(held, state->mv, held_count * sizeof(Value));
            for (Value cursor = p_cons_ptr(roots[3])->cdr; p_type(cursor) == P_CONS;
                 cursor = p_cons_ptr(cursor)->cdr) {
                (void)eval_form(state, p_cons_ptr(cursor)->car, roots[1], false);
                if (p->error.code != P_OK) break;
            }
            if (p->error.code == P_OK) {
                state->mv_count = held_count;
                memcpy(state->mv, held, held_count * sizeof(Value));
            }
            p_root_restore(p, held_root);
            break;
        }
        if (symbol_named(p, roots[2], "MULTIPLE-VALUE-CALL")) {
            if (roots[3] == p->nil) { (void)p_error(p, P_ERR_ARITY, "MULTIPLE-VALUE-CALL requires a function"); break; }
            roots[4] = p_cons_ptr(roots[3])->car;
            /* MULTIPLE-VALUE-CALL's first operand is an evaluated form, so a
             * lexical value binding containing a function is valid here. */
            roots[5] = eval_form(state, roots[4], roots[1], false);
            if (p->error.code == P_OK)
                roots[5] = global_function_designator(state, roots[5]);
            if (p->error.code != P_OK) break;
            Value argv[ARGUMENT_LIMIT] = { 0u };
            size_t argc = 0u, argroot = p_root_save(p);
            if (!p_roots(p, argv, ARGUMENT_LIMIT)) { p_root_restore(p, argroot); break; }
            for (Value cursor = p_cons_ptr(roots[3])->cdr; p_type(cursor) == P_CONS;
                 cursor = p_cons_ptr(cursor)->cdr) {
                (void)eval_form(state, p_cons_ptr(cursor)->car, roots[1], false);
                if (p->error.code != P_OK) break;
                for (size_t j = 0u; j < state->mv_count; ++j) {
                    if (argc == ARGUMENT_LIMIT) { (void)p_error(p, P_ERR_STACK, "too many multiple-value arguments"); break; }
                    argv[argc++] = state->mv[j];
                }
                if (p->error.code != P_OK) break;
            }
            if (p->error.code == P_OK) result = apply_values(state, roots[5], argc, argv);
            p_root_restore(p, argroot);
            break;
        }
        if (symbol_named(p, roots[2], "THE") || symbol_named(p, roots[2], "LOCALLY") ||
            symbol_named(p, roots[2], "EVAL-WHEN")) {
            Value body = roots[3];
            if (symbol_named(p, roots[2], "THE")) body = p_type(body) == P_CONS ? p_cons_ptr(body)->cdr : p->nil;
            else if (symbol_named(p, roots[2], "LOCALLY")) {
                roots[6] = p_env_create(state, roots[1]);
                if (roots[6] == 0u ||
                    !bind_declared_special_markers(state, roots[6], roots[3])) break;
                roots[1] = roots[6];
            }
            else if (symbol_named(p, roots[2], "EVAL-WHEN")) {
                if (p_type(roots[3]) != P_CONS || p_type(p_cons_ptr(roots[3])->cdr) != P_CONS) {
                    (void)p_error(p, P_ERR_ARITY, "EVAL-WHEN requires situations and a body");
                    break;
                }
                Value situations = p_cons_ptr(roots[3])->car;
                bool execute = false;
                while (p_type(situations) == P_CONS) {
                    if (keyword_named(p, p_cons_ptr(situations)->car, "EXECUTE"))
                        execute = true;
                    situations = p_cons_ptr(situations)->cdr;
                }
                if (situations != p->nil) {
                    (void)p_error(p, P_ERR_TYPE, "EVAL-WHEN situations must be a proper list");
                    break;
                }
                body = p_cons_ptr(p_cons_ptr(roots[3])->cdr)->cdr;
                if (!execute) {
                    result = p->nil;
                    state->mv[0] = result; state->mv_count = 1u;
                    break;
                }
            }
            result = eval_sequence(state, body, roots[1], (roots[14] != p->nil)); break;
        }
        if (symbol_named(p, roots[2], "SETQ")) {
            Value cursor = roots[3];
            result = p->nil;
            while (p_type(cursor) == P_CONS) {
                Value name = p_cons_ptr(cursor)->car;
                cursor = p_cons_ptr(cursor)->cdr;
                if (p_type(name) != P_SYMBOL || p_type(cursor) != P_CONS) {
                    (void)p_error(p, P_ERR_ARITY, "SETQ requires symbol/value pairs"); break;
                }
                if ((p_symbol_ptr(name)->flags & P_CONSTANT) != 0u) {
                    (void)p_error(p, P_ERR_CONTROL, "constant symbol cannot be assigned");
                    p->error.datum = name;
                    break;
                }
                Value value = eval_form(state, p_cons_ptr(cursor)->car, roots[1], false);
                if (p->error.code != P_OK) break;
                /* A symbol macro in SETQ names the variable to assign. The
                 * stage-0 subset supports symbol expansions to another symbol;
                 * generalized places are expanded by later boot code. */
                Value symbol_expansion = lookup_symbol_macro(state, roots[1], name);
                if (symbol_expansion != 0u) {
                    if (p_type(symbol_expansion) != P_SYMBOL) {
                        (void)p_error(p, P_ERR_UNSUPPORTED,
                                      "SETQ symbol macro must expand to a symbol");
                        p->error.datum = name;
                        break;
                    }
                    name = symbol_expansion;
                }
                if (p_env_set(state, roots[1], P_ENV_VALUE, name, value)) { }
                else p_symbol_ptr(name)->value = value;
                result = value;
                cursor = p_cons_ptr(cursor)->cdr;
            }
            state->mv[0] = result; state->mv_count = 1u; break;
        }
        if (symbol_named(p, roots[2], "FUNCTION")) {
            Value designator = list_nth(p, roots[3], 0u);
            if (p_type(designator) == P_CONS && symbol_named(p, p_cons_ptr(designator)->car, "LAMBDA")) {
                Value tail = p_cons_ptr(designator)->cdr;
                Value params = p_type(tail) == P_CONS ? p_cons_ptr(tail)->car : p->nil;
                Value body = p_type(tail) == P_CONS ? p_cons_ptr(tail)->cdr : p->nil;
                result = make_closure_at_safe_point(state, params, body, roots[1], false, p->nil);
            } else result = function_value(state, designator, roots[1]);
            state->mv[0] = result; state->mv_count = 1u; break;
        }
        if (symbol_named(p, roots[2], "LAMBDA")) {
            if (p_type(roots[3]) != P_CONS) { (void)p_error(p, P_ERR_ARITY, "LAMBDA requires parameters"); break; }
            roots[4] = p_cons_ptr(roots[3])->car;
            roots[5] = p_cons_ptr(roots[3])->cdr;
            result = make_closure_at_safe_point(state, roots[4], roots[5], roots[1], false, p->nil);
            state->mv[0] = result; state->mv_count = 1u; break;
        }
        if (symbol_named(p, roots[2], "LET") || symbol_named(p, roots[2], "LET*")) {
            bool sequential = symbol_named(p, roots[2], "LET*");
            if (!proper_list(p, roots[3], NULL) || roots[3] == p->nil) {
                (void)p_error(p, P_ERR_ARITY, "LET requires a binding list"); break;
            }
            roots[4] = p_cons_ptr(roots[3])->car;
            roots[5] = p_cons_ptr(roots[3])->cdr;
            size_t binding_count = 0u;
            if (!proper_list(p, roots[4], &binding_count) || binding_count > 256u) {
                (void)p_error(p, P_ERR_STACK, "LET has too many bindings"); break;
            }
            Value names[256] = { 0u }, values[256] = { 0u };
            size_t bind_saved = p_root_save(p);
            if (!p_roots(p, names, binding_count) || !p_roots(p, values, binding_count)) {
                p_root_restore(p, bind_saved); break;
            }
            roots[6] = p_env_create(state, roots[1]);
            if (roots[6] == 0u) { p_root_restore(p, bind_saved); break; }
            size_t dynamic_base = internals(state)->dynamic_count;
            bool has_dynamic_binding = false;
            Value bindings = roots[4];
            size_t i = 0u;
            while (p_type(bindings) == P_CONS && p->error.code == P_OK) {
                Value binding = p_cons_ptr(bindings)->car;
                Value name = p_type(binding) == P_CONS ? p_cons_ptr(binding)->car : binding;
                Value init = p_type(binding) == P_CONS &&
                             p_type(p_cons_ptr(binding)->cdr) == P_CONS ?
                             p_cons_ptr(p_cons_ptr(binding)->cdr)->car : p->nil;
                if (p_type(name) != P_SYMBOL) {
                    (void)p_error(p, P_ERR_TYPE, "LET binding name must be a symbol"); break;
                }
                if ((p_symbol_ptr(name)->flags & P_CONSTANT) != 0u) {
                    (void)p_error(p, P_ERR_CONTROL, "constant symbol cannot be bound");
                    p->error.datum = name;
                    break;
                }
                names[i] = name;
                values[i] = eval_form(state, init, sequential ? roots[6] : roots[1], false);
                if (p->error.code != P_OK) break;
                bool special = (p_symbol_ptr(name)->flags & P_SPECIAL) != 0u ||
                               is_declared_special(p, roots[5], name);
                if (sequential) {
                    /* LET* establishes a new lexical contour for each
                     * binding. Closures created by an initializer capture the
                     * prior contour, so later shadowing cannot rewrite it. */
                    roots[7] = p_env_create(state, roots[6]);
                    if (roots[7] == 0u) break;
                    roots[6] = roots[7];
                    if (special) {
                        Value old = p_symbol_ptr(name)->value;
                        if (!dynamic_push(state, DYNAMIC_SPECIAL, name, old, 0u, 0u)) break;
                        p_symbol_ptr(name)->value = values[i];
                        if (!env_bind_special_marker(state, roots[6], name)) break;
                        has_dynamic_binding = true;
                    } else if (!p_env_bind(state, roots[6], P_ENV_VALUE,
                                           name, values[i], false)) break;
                }
                ++i;
                bindings = p_cons_ptr(bindings)->cdr;
            }
            if (!sequential && p->error.code == P_OK) {
                for (size_t j = 0u; j < binding_count; ++j) {
                    bool special = (p_symbol_ptr(names[j])->flags & P_SPECIAL) != 0u ||
                                   is_declared_special(p, roots[5], names[j]);
                    if (special) {
                        Value old = p_symbol_ptr(names[j])->value;
                        if (!dynamic_push(state, DYNAMIC_SPECIAL, names[j], old, 0u, 0u)) break;
                        p_symbol_ptr(names[j])->value = values[j];
                        if (!env_bind_special_marker(state, roots[6], names[j])) break;
                        has_dynamic_binding = true;
                    } else if (!p_env_bind(state, roots[6], P_ENV_VALUE,
                                           names[j], values[j], false)) break;
                }
            }
            if (p->error.code == P_OK &&
                !bind_declared_special_markers(state, roots[6], roots[5])) { }
            if (p->error.code == P_OK)
                result = eval_sequence(state, roots[5], roots[6],
                                       (roots[14] != p->nil) && !has_dynamic_binding);
            (void)unwind_dynamic(state, dynamic_base);
            p_root_restore(p, bind_saved);
            break;
        }
        if (symbol_named(p, roots[2], "FLET") || symbol_named(p, roots[2], "LABELS") ||
            symbol_named(p, roots[2], "MACROLET") || symbol_named(p, roots[2], "SYMBOL-MACROLET")) {
            bool labels = symbol_named(p, roots[2], "LABELS");
            bool macrolet = symbol_named(p, roots[2], "MACROLET");
            bool symbol_macrolet = symbol_named(p, roots[2], "SYMBOL-MACROLET");
            if (p_type(roots[3]) != P_CONS) {
                (void)p_error(p, P_ERR_ARITY, "local binding form requires a binding list"); break;
            }
            roots[4] = p_cons_ptr(roots[3])->car;
            roots[5] = p_cons_ptr(roots[3])->cdr;
            size_t defs = 0u;
            if (!proper_list(p, roots[4], &defs) || defs > 256u) {
                (void)p_error(p, P_ERR_STACK, "too many local definitions"); break;
            }
            roots[6] = p_env_create(state, roots[1]);
            if (roots[6] == 0u) break;
            Value curs = roots[4];
            for (size_t i = 0u; i < defs && p->error.code == P_OK; ++i) {
                Value def = p_cons_ptr(curs)->car;
                if (p_type(def) != P_CONS || p_type(p_cons_ptr(def)->car) != P_SYMBOL ||
                    p_type(p_cons_ptr(def)->cdr) != P_CONS) {
                    (void)p_error(p, P_ERR_TYPE, "malformed local definition"); break;
                }
                Value name = p_cons_ptr(def)->car;
                Value rest = p_cons_ptr(def)->cdr;
                if (symbol_macrolet) {
                    Value expansion = p_cons_ptr(rest)->car;
                    if (!p_env_bind(state, roots[6], P_ENV_SYMBOL_MACRO,
                                    name, expansion, false)) break;
                } else if (!macrolet && !symbol_macrolet) {
                    Value params = p_cons_ptr(rest)->car;
                    Value body = p_cons_ptr(rest)->cdr;
                    Value captured = labels ? roots[6] : roots[1];
                    Value closure = make_closure_at_safe_point(state, params, body,
                                                                captured, false, name);
                    if (closure == 0u || !p_env_bind(state, roots[6], P_ENV_FUNCTION,
                                                     name, closure, false)) break;
                }
                curs = p_cons_ptr(curs)->cdr;
            }
            if (p->error.code == P_OK && macrolet) {
                curs = roots[4];
                for (size_t i = 0u; i < defs && p->error.code == P_OK; ++i) {
                    Value def = p_cons_ptr(curs)->car;
                    Value name = p_cons_ptr(def)->car;
                    Value rest = p_cons_ptr(def)->cdr;
                    Value closure = make_closure_at_safe_point(state,
                        p_cons_ptr(rest)->car, p_cons_ptr(rest)->cdr,
                        roots[6], true, name);
                    if (closure == 0u || !p_env_bind(state, roots[6], P_ENV_MACRO,
                                                     name, closure, false)) break;
                    curs = p_cons_ptr(curs)->cdr;
                }
            }
            if (p->error.code == P_OK)
                result = eval_sequence(state, roots[5], roots[6], (roots[14] != p->nil));
            break;
        }
        /* A call first checks a macro function in the lexical/global function
         * namespaces. Macro arguments are the original form and roots[1]. */
        bool local_function = p_type(roots[2]) == P_SYMBOL &&
                              p_env_lookup(state, roots[1], P_ENV_FUNCTION,
                                           roots[2]) != 0u;
        bool evaluator_call = !local_function &&
                              (symbol_named(p, roots[2], "VALUES") ||
                               symbol_named(p, roots[2], "FUNCALL") ||
                               symbol_named(p, roots[2], "APPLY"));
        if (p_type(roots[2]) == P_SYMBOL && !evaluator_call) {
            roots[8] = p_macro_function(state, roots[1], roots[2]);
            if (roots[8] != 0u) {
                Value macro_args[ARGUMENT_LIMIT] = { 0u };
                macro_args[0] = form;
                macro_args[1] = roots[1];
                size_t macro_argc = 2u;
                size_t macro_saved = p_root_save(p);
                if (!p_roots(p, macro_args, ARGUMENT_LIMIT)) { p_root_restore(p, macro_saved); break; }
                roots[0] = apply_values(state, roots[8], macro_argc, macro_args);
                p_root_restore(p, macro_saved);
                if (p->error.code != P_OK) break;
                continue;
            }
            roots[8] = p_env_lookup(state, roots[1], P_ENV_FUNCTION, roots[2]);
            if (roots[8] == 0u) roots[8] = p_symbol_ptr(roots[2])->function;
        } else if (evaluator_call) roots[8] = p->nil;
        else roots[8] = eval_form(state, roots[2], roots[1], false);
        if (p->error.code != P_OK) break;
        if (!evaluator_call && p_type(roots[8]) == P_SYMBOL)
            roots[8] = function_value(state, roots[8], roots[1]);
        if (p->error.code != P_OK) break;
        roots[9] = p_cons_ptr(form)->cdr;
        Value argv[ARGUMENT_LIMIT] = { 0u };
        size_t argc = 0u, args_saved = p_root_save(p);
        if (!p_roots(p, argv, ARGUMENT_LIMIT)) { p_root_restore(p, args_saved); break; }
        if (eval_arguments(state, roots[9], roots[1], argv, &argc) == 0u && p->error.code != P_OK) {
            p_root_restore(p, args_saved); break;
        }
        if (evaluator_call && symbol_named(p, roots[2], "VALUES")) {
            if (argc > P_MULTIPLE_VALUES_LIMIT) {
                (void)p_error(p, P_ERR_STACK, "too many multiple values");
                p_root_restore(p, args_saved); break;
            }
            state->mv_count = argc;
            if (argc == 0u) { state->mv[0] = p->nil; state->mv_count = 0u; }
            else memcpy(state->mv, argv, argc * sizeof(Value));
            result = argc == 0u ? p->nil : argv[0];
            p_root_restore(p, args_saved);
            break;
        }
        if (evaluator_call && symbol_named(p, roots[2], "FUNCALL")) {
            if (argc == 0u) { (void)arity_error(state, 1u, P_EVAL_PRIMITIVE_VARIADIC, 0u); p_root_restore(p, args_saved); break; }
            Value fn = global_function_designator(state, argv[0]);
            if (p->error.code != P_OK) {
                p_root_restore(p, args_saved);
                break;
            }
            if (roots[14] != p->nil && p_type(fn) == P_CLOSURE) {
                request_tail_call(state, fn, argc - 1u, &argv[1]);
                result = p->nil;
            } else result = apply_values(state, fn, argc - 1u, &argv[1]);
            p_root_restore(p, args_saved);
            break;
        }
        if (evaluator_call && symbol_named(p, roots[2], "APPLY")) {
            if (argc < 2u) { (void)arity_error(state, 2u, P_EVAL_PRIMITIVE_VARIADIC, argc); p_root_restore(p, args_saved); break; }
            Value expanded[ARGUMENT_LIMIT] = { 0u };
            size_t n = 0u;
            for (size_t i = 1u; i + 1u < argc; ++i) {
                if (n == ARGUMENT_LIMIT) break;
                expanded[n++] = argv[i];
            }
            Value tail = argv[argc - 1u];
            while (p_type(tail) == P_CONS && n < ARGUMENT_LIMIT) {
                expanded[n++] = p_cons_ptr(tail)->car;
                tail = p_cons_ptr(tail)->cdr;
            }
            if (tail != p->nil) (void)p_error(p, P_ERR_TYPE, "APPLY final argument must be a proper list");
            else {
                Value target = global_function_designator(state, argv[0]);
                if (p->error.code != P_OK) {
                    p_root_restore(p, args_saved);
                    break;
                }
                if (roots[14] != p->nil && p_type(target) == P_CLOSURE) {
                    request_tail_call(state, target, n, expanded);
                    result = p->nil;
                } else result = apply_values(state, target, n, expanded);
            }
            p_root_restore(p, args_saved);
            break;
        }
        if ((roots[14] != p->nil) && p_type(roots[8]) == P_CLOSURE) {
            if (argc > 512u) {
                (void)p_error(p, P_ERR_STACK, "too many tail-call arguments");
                p_root_restore(p, args_saved); break;
            }
            request_tail_call(state, roots[8], argc, argv);
            result = p->nil;
        } else result = apply_values(state, roots[8], argc, argv);
        p_root_restore(p, args_saved);
        break;
    }
    if (result == 0u && p->error.code == P_OK)
        (void)p_error(p, P_ERR_UNSUPPORTED, "stage-0 evaluation failed");
    if (p->error.code != P_OK) result = 0u;
    --state->call_depth;
    state->current_env = roots[15];
    p_root_restore(p, root_saved);
    return result;
}

Value p_eval0(PState *state, Value form, Value environment) {
    if (state == NULL || !state->initialized) return 0u;
    if (state->p->error.code != P_OK) return 0u;
    Value roots[3] = { form, environment, state->current_env };
    size_t saved = p_root_save(state->p);
    if (!p_roots(state->p, roots, 3u)) { p_root_restore(state->p, saved); return 0u; }
    if (roots[1] == 0u) {
        roots[1] = p_env_create(state, state->p->nil);
        if (roots[1] == 0u) { p_root_restore(state->p, saved); return 0u; }
    }
    state->current_env = roots[1];
    state->tail_pending = false;
    Value result = eval_form(state, roots[0], roots[1], true);
    if (state->tail_pending && state->p->error.code == P_OK) {
        result = apply_values(state, state->tail_function, state->tail_argc,
                              state->tail_arguments);
        state->tail_pending = false;
    }
    if (state->p->error.code != P_OK) result = 0u;
    state->current_env = roots[2];
    p_root_restore(state->p, saved);
    return result;
}

Value p_macro_function(PState *state, Value environment, Value name) {
    if (p_type(name) != P_SYMBOL) return 0u;
    for (Value env = environment; p_type(env) == P_RECORD &&
         ((PRecord *)env)->type == ENV_TAG; env = ((PRecord *)env)->data[0]) {
        if (find_local_binding(env, P_ENV_FUNCTION, name) != 0u) return 0u;
        Value binding = find_local_binding(env, P_ENV_MACRO, name);
        if (binding != 0u) return p_cons_ptr(binding)->cdr;
    }
    (void)state;
    Value function = p_symbol_ptr(name)->function;
    if (p_type(function) == P_CLOSURE &&
        ((((PClosure *)function)->flags & P_EVAL_MACRO) != 0u ||
         (p_symbol_ptr(name)->flags & P_MACRO) != 0u))
        return function;
    return 0u;
}

Value p_macroexpand1(PState *state, Value form, Value environment,
                     bool *expanded) {
    if (expanded != NULL) *expanded = false;
    if (p_type(form) != P_CONS) return form;
    Value name = p_cons_ptr(form)->car;
    if (p_type(name) != P_SYMBOL) return form;
    Value macro = p_macro_function(state, environment, name);
    if (macro == 0u) return form;
    Value roots[3] = { form, environment, macro };
    size_t saved = p_root_save(state->p);
    if (!p_roots(state->p, roots, 3u) || !p_reserve(state->p, 2u * sizeof(PCons))) {
        p_root_restore(state->p, saved); return 0u;
    }
    Value args[ARGUMENT_LIMIT] = { 0u };
    size_t argc = 2u;
    args[0] = roots[0];
    args[1] = roots[1];
    size_t args_saved = p_root_save(state->p);
    if (!p_roots(state->p, args, ARGUMENT_LIMIT)) { p_root_restore(state->p, saved); return 0u; }
    Value result = apply_values(state, roots[2], argc, args);
    p_root_restore(state->p, args_saved);
    p_root_restore(state->p, saved);
    if (expanded != NULL && state->p->error.code == P_OK) *expanded = true;
    return result;
}
