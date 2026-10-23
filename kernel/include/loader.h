#ifndef POCKET_LOADER_H
#define POCKET_LOADER_H

#include "vm.h"

#define P_FASL_VERSION 1u
#define P_FASL_COMPILER 1u
#define P_FASL_DEBUG 2u
#define P_FASL_FIXNUM31 256u

/* Preflight runs outside a primitive entry, before p_reserve. The load itself
 * never collects or executes Lisp and returns a verified entry closure. The
 * caller roots the input byte vector across both phases. */
bool p_fasl_required_bytes(Pocket *p, Value bytes, size_t *required);
Value p_fasl_load(Pocket *p, Value bytes);
bool p_install_fasl_primitives(PState *state);

#endif
