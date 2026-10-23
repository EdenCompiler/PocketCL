#include "loader.h"

#include <float.h>
#include <string.h>

/* FASL primitives are intentionally small adapters. The loader does not
 * evaluate Lisp or collect; the evaluator invokes the reserve callback at a
 * safe point while the byte vector argument is rooted. */
static bool reserve_fasl_load(PState *state, size_t argc, const Value *argv,
                              size_t *bytes)
{
    (void)argc;
    return p_fasl_required_bytes(state->p, argv[0], bytes);
}

static Value primitive_load_fasl(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return p_fasl_load(state->p, argv[0]);
}

static bool reserve_flonum_bytes(PState *state, size_t argc,
                                 const Value *argv, size_t *bytes)
{
    (void)argc;
    (void)argv;
    (void)state;
    *bytes = offsetof(PBytes, data) + 8u;
    return true;
}

static Value primitive_flonum_bytes(PState *state, size_t argc,
                                    const Value *argv)
{
    uint64_t bits;
    Value result;
    double number;
    (void)argc;
    if (p_type(argv[0]) != P_FLONUM) {
        (void)p_error(state->p, P_ERR_TYPE, "%FLONUM-BYTES requires a float");
        return 0u;
    }
    if (sizeof(double) != 8u || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024) {
        (void)p_error(state->p, P_ERR_UNSUPPORTED,
                      "runtime does not support IEEE binary64 floats");
        return 0u;
    }
    number = ((PFlonum *)argv[0])->number;
    memcpy(&bits, &number, sizeof(bits));
    result = p_bytes(state->p, 8u);
    if (result == 0u)
        return 0u;
    for (unsigned i = 0u; i < 8u; ++i)
        ((PBytes *)result)->data[i] = (uint8_t)(bits >> (8u * i));
    return result;
}

bool p_install_fasl_primitives(PState *state)
{
    static const struct {
        const char *name;
        PPrimitiveFn entry;
        PPrimitiveReserveFn reserve;
    } specs[] = {
        { "%LOAD-FASL", primitive_load_fasl, reserve_fasl_load },
        { "%FLONUM-BYTES", primitive_flonum_bytes, reserve_flonum_bytes }
    };
    if (state == NULL || !state->initialized || state->p == NULL)
        return false;
    for (size_t i = 0u; i < sizeof(specs) / sizeof(specs[0]); ++i) {
        if (!p_add_primitive(state, specs[i].name, false, specs[i].entry,
                             specs[i].reserve, 1u, 1u, 0u))
            return false;
    }
    return true;
}
