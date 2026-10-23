#ifndef POCKET_EXEC_H
#define POCKET_EXEC_H

#include "eval.h"

/* Shared engine support. Environments are ordinary GC records whose first
 * slot is the parent and whose next six slots hold the lexical namespaces.
 * Keeping their layout here lets both the tree evaluator and portable VM use
 * the same bindings without linking interpreter control machinery. */
#define P_EXEC_ENV_TAG p_fixnum(-73)
#define P_EXEC_ENV_SLOTS 7u

bool p_state_init(PState *state, Pocket *p);
void p_state_destroy(PState *state);
Value p_exec_env_binding(Value environment, PEnvNamespace space, Value name);
bool p_exec_env_bind_special_marker(PState *state, Value environment,
                                    Value name);
bool p_exec_seed_core_symbols(Pocket *p);

#endif
