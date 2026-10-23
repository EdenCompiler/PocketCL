#ifndef POCKET_EVAL_H
#define POCKET_EVAL_H

#include "pocket.h"

#define P_MULTIPLE_VALUES_LIMIT 64u
#define P_EVAL_ROOT_LIMIT 2048u
#define P_EVAL_DEPTH_LIMIT 256u
#define P_EVAL_MACRO 1u
#define P_EVAL_LOCAL_MACRO 2u
#define P_EVAL_PRIMITIVE_VARIADIC ((size_t)-1)

enum {
    P_PRIMITIVE_EVAL_FUNCALL = 1u,
    P_PRIMITIVE_EVAL_APPLY = 2u,
    P_PRIMITIVE_EVAL_VALUES = 4u
};

typedef struct PState PState;
typedef Value (*PStateApplyFn)(PState *state, Value function, size_t argc,
                               const Value *argv);

/* A tree closure owns its lambda list, body and lexical environment. The
 * compiler may append code metadata later while preserving the object prefix. */
typedef struct {
    uintptr_t header;
    Value name, parameters, body, environment;
    size_t required_count;
    Value rest_parameter;
    unsigned flags;
} PClosure;

typedef Value (*PPrimitiveFn)(PState *state, size_t argc, const Value *argv);
typedef bool (*PPrimitiveReserveFn)(PState *state, size_t argc,
                                    const Value *argv, size_t *bytes);

/* Primitive entries do not evaluate Lisp or collect. An optional reserve
 * estimator lets the evaluator collect safely while arguments are rooted. */
typedef struct {
    uintptr_t header;
    Value name;
    PPrimitiveFn entry;
    PPrimitiveReserveFn reserve;
    size_t min_arity, max_arity;
    unsigned flags;
} PPrimitive;

struct PState {
    Pocket *p;
    Value mv[P_MULTIPLE_VALUES_LIMIT];
    size_t mv_count;
    Value current_env;

    /* Public for root_main and primitive implementations. Private state is
     * opaque so C stack frames never contain unregistered moving Lisp Values. */
    Value control_owner;
    Value roots[P_EVAL_ROOT_LIMIT];
    Value tail_function;
    Value tail_arguments[512];
    /* Bridge hooks are engine dispatch, never native-primitive callbacks.
     * The final runtime leaves interpreted_apply NULL; stage 0 installs it
     * so its tree evaluator and compiled bootstrap closures can interoperate. */
    PStateApplyFn interpreted_apply;
    void *vm_engine;
    void *control;
    size_t tail_argc, control_capacity, call_depth, max_call_depth, saved_root_count;
    size_t root_used;
    uint64_t next_control_token;
    bool tail_pending, initialized;
};

typedef enum {
    P_ENV_VALUE, P_ENV_FUNCTION, P_ENV_MACRO, P_ENV_SYMBOL_MACRO,
    P_ENV_BLOCK, P_ENV_TAG
} PEnvNamespace;

void p_eval_init(PState *state, Pocket *p);
void p_eval_destroy(PState *state);
Value p_eval0(PState *state, Value form, Value environment);
Value p_apply0(PState *state, Value function, Value arguments);
Value p_make_closure(PState *state, Value parameters, Value body,
                     Value environment, bool macro, Value name);
Value p_macro_function(PState *state, Value environment, Value name);
Value p_macroexpand1(PState *state, Value form, Value environment,
                     bool *expanded);

Value p_env_create(PState *state, Value parent);
Value p_env_lookup(PState *state, Value environment,
                   PEnvNamespace space, Value name);
bool p_env_bind(PState *state, Value environment, PEnvNamespace space,
               Value name, Value value, bool replace);
bool p_env_set(PState *state, Value environment, PEnvNamespace space,
               Value name, Value value);

/* The primitive table is installed by the primitive subsystem after state
 * initialization; keeping that step explicit lets tests use an empty image. */
bool p_install_primitives(PState *state);
/* Register one C primitive into CL (standard=true) or SYS. The helper
 * reserves before mutating package/function state; native entries must still
 * be non-collecting and must not call back into Lisp. */
bool p_add_primitive(PState *state, const char *name, bool standard,
                     PPrimitiveFn entry, PPrimitiveReserveFn reserve,
                     size_t min_arity, size_t max_arity, unsigned flags);
/* Check an inline operation's fallback function cell against its original
 * native implementation. Redefinitions take the VM's ordinary call path. */
bool p_is_builtin_primitive(Pocket *p, Value symbol, const char *name);

#endif
