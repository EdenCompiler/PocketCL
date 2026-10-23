#include "eval.h"
#include "float_io.h"
#include "host.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PRIMITIVE_NAME_CAPACITY 4096u
#define CONSOLE_LINE_CAPACITY 4096u

static Value fail(PState *state, PErrorCode code, const char *message)
{
    state->mv_count = 0u;
    (void)p_error(state->p, code, message);
    return 0;
}

static Value one(PState *state, Value value)
{
    state->mv[0] = value;
    state->mv_count = 1u;
    return value;
}

static Value floating_value(PState *state, double number)
{
    Value value = p_flonum(state->p, number);
    return value ? one(state, value) : fail(state, state->p->error.code,
                                             "floating-point allocation failed");
}

static bool fixnum_arg(PState *state, Value value, int64_t *result)
{
    if (!p_is_fixnum(value)) {
        (void)p_error(state->p, P_ERR_TYPE, "expected a fixnum integer");
        return false;
    }
    *result = p_fixnum_value(value);
    return true;
}

static bool index_arg(PState *state, Value value, size_t limit, size_t *index)
{
    int64_t number;
    if (!fixnum_arg(state, value, &number))
        return false;
    if (number < 0 || (uint64_t)number >= (uint64_t)limit) {
        (void)p_error(state->p, P_ERR_RANGE, "index is outside the sequence bounds");
        return false;
    }
    *index = (size_t)number;
    return true;
}

static bool numeric_arg(PState *state, Value value, double *number,
                        bool *is_integer)
{
    if (p_is_fixnum(value)) {
        *number = (double)p_fixnum_value(value);
        *is_integer = true;
        return true;
    }
    if (p_type(value) == P_FLONUM) {
        *number = ((PFlonum *)value)->number;
        *is_integer = false;
        return true;
    }
    (void)p_error(state->p, P_ERR_TYPE, "expected a real number");
    return false;
}

static bool reserve_cons(PState *state, size_t argc, const Value *argv,
                         size_t *bytes)
{
    (void)state; (void)argc; (void)argv;
    *bytes = sizeof(PCons);
    return true;
}

static bool reserve_flonum(PState *state, size_t argc, const Value *argv,
                           size_t *bytes)
{
    size_t i;
    (void)state;
    for (i = 0; i < argc; ++i) {
        if (p_type(argv[i]) == P_FLONUM) {
            *bytes = sizeof(PFlonum);
            return true;
        }
    }
    *bytes = 0u;
    return true;
}

static bool reserve_float_result(PState *state, size_t argc, const Value *argv,
                                 size_t *bytes)
{
    (void)state; (void)argc; (void)argv;
    *bytes = sizeof(PFlonum);
    return true;
}

static bool reserve_vector(PState *state, size_t argc, const Value *argv,
                           size_t *bytes)
{
    int64_t count;
    size_t header = offsetof(PVector, data);
    if (argc == 0u || !fixnum_arg(state, argv[0], &count))
        return false;
    if (count < 0 || (uint64_t)count > (SIZE_MAX - header) / sizeof(Value)) {
        (void)p_error(state->p, P_ERR_RANGE, "simple-vector length is out of range");
        return false;
    }
    *bytes = header + (size_t)count * sizeof(Value);
    return true;
}

static bool reserve_string(PState *state, size_t argc, const Value *argv,
                           size_t *bytes)
{
    int64_t count;
    size_t header = offsetof(PString, data);
    if (argc == 0u || !fixnum_arg(state, argv[0], &count))
        return false;
    if (count < 0 || (uint64_t)count > (SIZE_MAX - header) / sizeof(uint32_t)) {
        (void)p_error(state->p, P_ERR_RANGE, "string length is out of range");
        return false;
    }
    *bytes = header + (size_t)count * sizeof(uint32_t);
    return true;
}

static bool reserve_bytes_vector(PState *state, size_t argc, const Value *argv,
                                 size_t *bytes)
{
    int64_t count;
    size_t header = offsetof(PBytes, data);
    if (argc == 0u || !fixnum_arg(state, argv[0], &count))
        return false;
    if (count < 0 || (uint64_t)count > SIZE_MAX - header) {
        (void)p_error(state->p, P_ERR_RANGE, "byte-vector length is out of range");
        return false;
    }
    *bytes = header + (size_t)count;
    return true;
}

static bool reserve_record(PState *state, size_t argc, const Value *argv,
                           size_t *bytes)
{
    int64_t count;
    size_t header = offsetof(PRecord, data);
    if (argc < 2u || !fixnum_arg(state, argv[1], &count))
        return false;
    if (count < 0 || (uint64_t)count > (SIZE_MAX - header) / sizeof(Value)) {
        (void)p_error(state->p, P_ERR_RANGE, "record length is out of range");
        return false;
    }
    *bytes = header + (size_t)count * sizeof(Value);
    return true;
}

static bool reserve_intern(PState *state, size_t argc, const Value *argv,
                           size_t *bytes)
{
    size_t length;
    (void)argc;
    if (p_type(argv[0]) != P_STRING) {
        (void)p_error(state->p, P_ERR_TYPE, "symbol name must be a string");
        return false;
    }
    length = p_string_ptr(argv[0])->length;
    if (length > (SIZE_MAX - 1024u) / 8u) {
        (void)p_error(state->p, P_ERR_RANGE, "symbol name is too long");
        return false;
    }
    *bytes = 1024u + length * 8u;
    return true;
}

static bool reserve_package(PState *state, size_t argc, const Value *argv,
                            size_t *bytes)
{
    size_t length;
    (void)argc;
    if (p_type(argv[0]) != P_STRING) {
        (void)p_error(state->p, P_ERR_TYPE, "package name must be a string");
        return false;
    }
    length = p_string_ptr(argv[0])->length;
    if (length > (SIZE_MAX - 1024u) / 8u) {
        (void)p_error(state->p, P_ERR_RANGE, "package name is too long");
        return false;
    }
    *bytes = 1024u + length * 8u;
    return true;
}

static bool reserve_host_read(PState *state, size_t argc, const Value *argv,
                              size_t *bytes)
{
    size_t length;
    (void)argc;
    if (!p_host_file_size(state->p, argv[0], &length))
        return false;
    if (length > SIZE_MAX - offsetof(PBytes, data)) {
        (void)p_error(state->p, P_ERR_RANGE, "file is too large for a byte vector");
        return false;
    }
    *bytes = offsetof(PBytes, data) + length;
    return true;
}

static bool reserve_console_line(PState *state, size_t argc, const Value *argv,
                                 size_t *bytes)
{
    (void)state; (void)argc; (void)argv;
    *bytes = offsetof(PString, data) + CONSOLE_LINE_CAPACITY * sizeof(uint32_t);
    return true;
}

static Value primitive_cons(PState *state, size_t argc, const Value *argv)
{
    Value value;
    (void)argc;
    value = p_cons(state->p, argv[0], argv[1]);
    return value ? one(state, value) : fail(state, state->p->error.code,
                                             "cons allocation failed");
}

static Value primitive_car(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    if (argv[0] == state->p->nil)
        return one(state, state->p->nil);
    if (p_type(argv[0]) != P_CONS)
        return fail(state, P_ERR_TYPE, "CAR requires a cons or NIL");
    return one(state, p_cons_ptr(argv[0])->car);
}

static Value primitive_cdr(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    if (argv[0] == state->p->nil)
        return one(state, state->p->nil);
    if (p_type(argv[0]) != P_CONS)
        return fail(state, P_ERR_TYPE, "CDR requires a cons or NIL");
    return one(state, p_cons_ptr(argv[0])->cdr);
}

static Value primitive_rplaca(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_CONS)
        return fail(state, P_ERR_TYPE, "RPLACA requires a cons");
    p_cons_ptr(argv[0])->car = argv[1];
    return one(state, argv[0]);
}

static Value primitive_rplacd(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_CONS)
        return fail(state, P_ERR_TYPE, "RPLACD requires a cons");
    p_cons_ptr(argv[0])->cdr = argv[1];
    return one(state, argv[0]);
}

static Value primitive_eq(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, argv[0] == argv[1] ? state->p->truth : state->p->nil);
}

static Value primitive_eql(PState *state, size_t argc, const Value *argv)
{
    bool equal = argv[0] == argv[1];
    (void)argc;
    if (!equal && p_type(argv[0]) == P_FLONUM && p_type(argv[1]) == P_FLONUM)
        equal = ((PFlonum *)argv[0])->number == ((PFlonum *)argv[1])->number;
    return one(state, equal ? state->p->truth : state->p->nil);
}

static Value primitive_atom(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_type(argv[0]) == P_CONS ? state->p->nil : state->p->truth);
}

static Value primitive_consp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_type(argv[0]) == P_CONS ? state->p->truth : state->p->nil);
}

static Value primitive_listp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, (argv[0] == state->p->nil || p_type(argv[0]) == P_CONS)
                      ? state->p->truth : state->p->nil);
}

static Value primitive_null(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, argv[0] == state->p->nil ? state->p->truth : state->p->nil);
}

static Value primitive_values(PState *state, size_t argc, const Value *argv)
{
    size_t i;
    if (argc > P_MULTIPLE_VALUES_LIMIT)
        return fail(state, P_ERR_RANGE, "too many multiple values");
    for (i = 0u; i < argc; ++i)
        state->mv[i] = argv[i];
    state->mv_count = argc;
    return argc == 0u ? state->p->nil : argv[0];
}

static Value primitive_symbolp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_type(argv[0]) == P_SYMBOL ? state->p->truth : state->p->nil);
}

static Value primitive_numberp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, (p_is_fixnum(argv[0]) || p_type(argv[0]) == P_FLONUM)
                      ? state->p->truth : state->p->nil);
}

static Value primitive_integerp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_is_fixnum(argv[0]) ? state->p->truth : state->p->nil);
}

static Value primitive_floatp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_type(argv[0]) == P_FLONUM ? state->p->truth : state->p->nil);
}

static Value primitive_characterp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_is_character(argv[0]) ? state->p->truth : state->p->nil);
}

static Value primitive_stringp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_type(argv[0]) == P_STRING ? state->p->truth : state->p->nil);
}

static Value primitive_vectorp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, (p_type(argv[0]) == P_VECTOR || p_type(argv[0]) == P_STRING ||
                       p_type(argv[0]) == P_BYTES) ? state->p->truth : state->p->nil);
}

static Value primitive_simple_vectorp(PState *state, size_t argc,
                                      const Value *argv)
{
    (void)argc;
    return one(state, p_type(argv[0]) == P_VECTOR
                      ? state->p->truth : state->p->nil);
}

static Value primitive_functionp(PState *state, size_t argc, const Value *argv)
{
    PType type;
    (void)argc;
    type = p_type(argv[0]);
    return one(state, (type == P_CLOSURE || type == P_PRIMITIVE)
                      ? state->p->truth : state->p->nil);
}

static Value primitive_packagep(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_type(argv[0]) == P_PACKAGE
                      ? state->p->truth : state->p->nil);
}

static Value integer_value(PState *state, int64_t value)
{
    Value encoded = p_checked_fixnum(state->p, value);
    return encoded ? one(state, encoded) : fail(state, state->p->error.code,
                                                 "integer result is out of range");
}

static bool checked_add(int64_t a, int64_t b, int64_t *result)
{
    if ((b > 0 && a > p_fixnum_max() - b) ||
        (b < 0 && a < p_fixnum_min() - b))
        return false;
    *result = a + b;
    return true;
}

static bool checked_subtract(int64_t a, int64_t b, int64_t *result)
{
    if ((b < 0 && a > p_fixnum_max() + b) ||
        (b > 0 && a < p_fixnum_min() + b))
        return false;
    *result = a - b;
    return true;
}

static bool checked_multiply(int64_t a, int64_t b, int64_t *result)
{
    if (a == 0 || b == 0) { *result = 0; return true; }
    if ((a == -1 && b == p_fixnum_min()) ||
        (b == -1 && a == p_fixnum_min()))
        return false;
    if (a > 0) {
        if ((b > 0 && a > p_fixnum_max() / b) ||
            (b < 0 && b < p_fixnum_min() / a))
            return false;
    } else {
        if ((b > 0 && a < p_fixnum_min() / b) ||
            (b < 0 && a < p_fixnum_max() / b))
            return false;
    }
    *result = a * b;
    return true;
}

static Value primitive_add(PState *state, size_t argc, const Value *argv)
{
    size_t i;
    int64_t integer = 0, next;
    double floating = 0.0, number;
    bool is_integer, all_integer = true;
    for (i = 0; i < argc; ++i) {
        if (!numeric_arg(state, argv[i], &number, &is_integer))
            return 0;
        if (all_integer && is_integer) {
            if (!checked_add(integer, p_fixnum_value(argv[i]), &next))
                return fail(state, P_ERR_RANGE, "fixnum addition overflow");
            integer = next;
        } else {
            if (all_integer) { floating = (double)integer; all_integer = false; }
            floating += number;
        }
    }
    if (all_integer)
        return integer_value(state, integer);
    return floating_value(state, floating);
}

static Value primitive_subtract(PState *state, size_t argc, const Value *argv)
{
    size_t i;
    double first, number, floating;
    bool first_integer, is_integer, all_integer;
    int64_t integer, next;
    if (!numeric_arg(state, argv[0], &first, &first_integer))
        return 0;
    integer = first_integer ? p_fixnum_value(argv[0]) : 0;
    floating = first;
    all_integer = first_integer;
    if (argc == 1u) {
        if (all_integer) {
            if (integer == p_fixnum_min())
                return fail(state, P_ERR_RANGE, "fixnum negation overflow");
            return integer_value(state, -integer);
        }
        return floating_value(state, -floating);
    }
    for (i = 1u; i < argc; ++i) {
        if (!numeric_arg(state, argv[i], &number, &is_integer))
            return 0;
        if (all_integer && is_integer) {
            if (!checked_subtract(integer, p_fixnum_value(argv[i]), &next))
                return fail(state, P_ERR_RANGE, "fixnum subtraction overflow");
            integer = next;
        } else {
            if (all_integer) { floating = (double)integer; all_integer = false; }
            floating -= number;
        }
    }
    if (all_integer)
        return integer_value(state, integer);
    return floating_value(state, floating);
}

static Value primitive_multiply(PState *state, size_t argc, const Value *argv)
{
    size_t i;
    int64_t integer = 1, next;
    double floating = 1.0, number;
    bool is_integer, all_integer = true;
    for (i = 0; i < argc; ++i) {
        if (!numeric_arg(state, argv[i], &number, &is_integer))
            return 0;
        if (all_integer && is_integer) {
            if (!checked_multiply(integer, p_fixnum_value(argv[i]), &next))
                return fail(state, P_ERR_RANGE, "fixnum multiplication overflow");
            integer = next;
        } else {
            if (all_integer) { floating = (double)integer; all_integer = false; }
            floating *= number;
        }
    }
    if (all_integer)
        return integer_value(state, integer);
    return floating_value(state, floating);
}

static Value primitive_divide(PState *state, size_t argc, const Value *argv)
{
    size_t i;
    double numerator, divisor, floating;
    bool numerator_integer, divisor_integer, all_integer;
    int64_t integer;
    if (!numeric_arg(state, argv[0], &numerator, &numerator_integer))
        return 0;
    integer = numerator_integer ? p_fixnum_value(argv[0]) : 0;
    floating = numerator;
    all_integer = numerator_integer;
    if (argc == 1u) {
        if (all_integer) {
            if (integer == 0)
                return fail(state, P_ERR_RANGE, "division by zero");
            if (integer == 1 || integer == -1)
                return integer_value(state, integer);
            return fail(state, P_ERR_UNSUPPORTED,
                        "exact reciprocal requires ratios");
        }
        return floating_value(state, 1.0 / floating);
    }
    for (i = 1u; i < argc; ++i) {
        if (!numeric_arg(state, argv[i], &divisor, &divisor_integer))
            return 0;
        if (divisor == 0.0)
            return fail(state, P_ERR_RANGE, "division by zero");
        if (all_integer && divisor_integer) {
            int64_t d = p_fixnum_value(argv[i]);
            if (integer == p_fixnum_min() && d == -1)
                return fail(state, P_ERR_RANGE, "fixnum division overflow");
            if (integer % d != 0)
                return fail(state, P_ERR_UNSUPPORTED,
                            "non-integral exact division requires ratios");
            integer /= d;
        } else {
            if (all_integer) { floating = (double)integer; all_integer = false; }
            floating /= divisor;
        }
    }
    if (all_integer)
        return integer_value(state, integer);
    return floating_value(state, floating);
}

typedef enum { CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE } CompareKind;

static Value compare_numbers(PState *state, size_t argc, const Value *argv,
                            CompareKind kind)
{
    size_t i, j;
    Value first;
    if (kind == CMP_NE) {
        for (i = 0; i < argc; ++i) {
            double left;
            bool left_integer;
            if (!numeric_arg(state, argv[i], &left, &left_integer)) return 0;
            for (j = i + 1u; j < argc; ++j) {
                double right;
                bool right_integer;
                if (!numeric_arg(state, argv[j], &right, &right_integer)) return 0;
                if ((left_integer && right_integer &&
                     p_fixnum_value(argv[i]) == p_fixnum_value(argv[j])) ||
                    (!(left_integer && right_integer) && left == right))
                    return one(state, state->p->nil);
            }
        }
        return one(state, state->p->truth);
    }
    first = argv[0];
    for (i = 1u; i < argc; ++i) {
        double left, right;
        bool left_integer, right_integer, result;
        if (!numeric_arg(state, first, &left, &left_integer) ||
            !numeric_arg(state, argv[i], &right, &right_integer)) return 0;
        if (left_integer && right_integer) {
            int64_t a = p_fixnum_value(first), b = p_fixnum_value(argv[i]);
            switch (kind) {
            case CMP_EQ: result = a == b; break;
            case CMP_LT: result = a < b; break;
            case CMP_LE: result = a <= b; break;
            case CMP_GT: result = a > b; break;
            default: result = a >= b; break;
            }
        } else {
            switch (kind) {
            case CMP_EQ: result = left == right; break;
            case CMP_LT: result = left < right; break;
            case CMP_LE: result = left <= right; break;
            case CMP_GT: result = left > right; break;
            default: result = left >= right; break;
            }
        }
        if (!result)
            return one(state, state->p->nil);
        first = argv[i];
    }
    return one(state, state->p->truth);
}

static Value primitive_num_eq(PState *s, size_t n, const Value *a) { return compare_numbers(s, n, a, CMP_EQ); }
static Value primitive_num_ne(PState *s, size_t n, const Value *a) { return compare_numbers(s, n, a, CMP_NE); }
static Value primitive_num_lt(PState *s, size_t n, const Value *a) { return compare_numbers(s, n, a, CMP_LT); }
static Value primitive_num_le(PState *s, size_t n, const Value *a) { return compare_numbers(s, n, a, CMP_LE); }
static Value primitive_num_gt(PState *s, size_t n, const Value *a) { return compare_numbers(s, n, a, CMP_GT); }
static Value primitive_num_ge(PState *s, size_t n, const Value *a) { return compare_numbers(s, n, a, CMP_GE); }

typedef enum { ROUND_TRUNCATE, ROUND_FLOOR, ROUND_CEILING, ROUND_NEAREST } RoundKind;

static uint64_t magnitude_i64(int64_t value)
{
    return value < 0 ? (uint64_t)(-(value + 1)) + 1u : (uint64_t)value;
}

static Value quotient_remainder(PState *state, size_t argc, const Value *argv,
                                RoundKind kind)
{
    double a, b = 1.0, qd, rd;
    bool ai, bi = true;
    Value qvalue, rvalue;
    int64_t quotient, remainder;
    if (!numeric_arg(state, argv[0], &a, &ai))
        return 0;
    if (argc == 2u && !numeric_arg(state, argv[1], &b, &bi))
        return 0;
    if (b == 0.0)
        return fail(state, P_ERR_RANGE, "division by zero");
    if (ai && bi) {
        int64_t x = p_fixnum_value(argv[0]);
        int64_t y = argc == 2u ? p_fixnum_value(argv[1]) : 1;
        if (x == p_fixnum_min() && y == -1)
            return fail(state, P_ERR_RANGE, "fixnum quotient overflow");
        quotient = x / y;
        remainder = x % y;
        if (kind == ROUND_FLOOR && remainder != 0 &&
            ((remainder < 0) != (y < 0))) {
            --quotient;
            remainder += y;
        } else if (kind == ROUND_CEILING && remainder != 0 &&
                   ((remainder < 0) == (y < 0))) {
            ++quotient;
            remainder -= y;
        } else if (kind == ROUND_NEAREST && remainder != 0) {
            uint64_t remmag = magnitude_i64(remainder);
            uint64_t denmag = magnitude_i64(y);
            bool above_half = remmag > denmag - remmag;
            bool tie_odd = remmag == denmag - remmag && quotient % 2 != 0;
            if (above_half || tie_odd) {
                int64_t step = ((x < 0) == (y < 0)) ? 1 : -1;
                quotient += step;
                remainder -= step * y;
            }
        }
        qvalue = p_checked_fixnum(state->p, quotient);
        rvalue = p_checked_fixnum(state->p, remainder);
        if (!qvalue || !rvalue)
            return fail(state, state->p->error.code, "quotient result is out of range");
    } else {
        double ratio = a / b;
        switch (kind) {
        case ROUND_FLOOR: qd = floor(ratio); break;
        case ROUND_CEILING: qd = ceil(ratio); break;
        case ROUND_NEAREST: qd = nearbyint(ratio); break;
        default: qd = trunc(ratio); break;
        }
        if (!isfinite(qd) || qd < (double)p_fixnum_min() ||
            qd > (double)p_fixnum_max())
            return fail(state, P_ERR_RANGE, "quotient result is out of fixnum range");
        quotient = (int64_t)qd;
        rd = a - qd * b;
        qvalue = p_checked_fixnum(state->p, quotient);
        rvalue = p_flonum(state->p, rd);
        if (!qvalue || !rvalue)
            return fail(state, state->p->error.code, "quotient allocation failed");
    }
    state->mv[0] = qvalue;
    state->mv[1] = rvalue;
    state->mv_count = 2u;
    return qvalue;
}

static Value primitive_truncate(PState *s, size_t n, const Value *a)
{ return quotient_remainder(s, n, a, ROUND_TRUNCATE); }
static Value primitive_floor(PState *s, size_t n, const Value *a)
{ return quotient_remainder(s, n, a, ROUND_FLOOR); }
static Value primitive_ceiling(PState *s, size_t n, const Value *a)
{ return quotient_remainder(s, n, a, ROUND_CEILING); }
static Value primitive_round(PState *s, size_t n, const Value *a)
{ return quotient_remainder(s, n, a, ROUND_NEAREST); }

static Value primitive_rem_mod(PState *state, size_t argc, const Value *argv,
                               bool modulo)
{
    double a, b;
    bool ai, bi;
    Value result;
    if (!numeric_arg(state, argv[0], &a, &ai) ||
        !numeric_arg(state, argv[1], &b, &bi))
        return 0;
    if (b == 0.0)
        return fail(state, P_ERR_RANGE, "division by zero");
    if (ai && bi) {
        int64_t x = p_fixnum_value(argv[0]);
        int64_t y = p_fixnum_value(argv[1]);
        int64_t r = (x == p_fixnum_min() && y == -1) ? 0 : x % y;
        if (modulo && r != 0 && ((r < 0) != (y < 0)))
            r += y;
        return integer_value(state, r);
    }
    {
        double r = fmod(a, b);
        if (modulo && r != 0.0 && ((r < 0.0) != (b < 0.0)))
            r += b;
        result = p_flonum(state->p, r);
    }
    (void)argc;
    return result ? one(state, result) : fail(state, state->p->error.code,
                                               "remainder allocation failed");
}

static Value primitive_rem(PState *s, size_t n, const Value *a)
{ return primitive_rem_mod(s, n, a, false); }
static Value primitive_mod(PState *s, size_t n, const Value *a)
{ return primitive_rem_mod(s, n, a, true); }

static int64_t signed_from_twos(uint64_t bits)
{
    if (bits <= (uint64_t)INT64_MAX)
        return (int64_t)bits;
    return -1 - (int64_t)(~bits);
}

typedef enum { BIT_AND, BIT_IOR, BIT_XOR, BIT_NOT } BitKind;

static Value bit_operation(PState *state, size_t argc, const Value *argv,
                          BitKind kind)
{
    size_t i;
    uint64_t result;
    int64_t number;
    if (kind == BIT_AND)
        result = argc == 0u ? UINT64_MAX : (uint64_t)p_fixnum_value(argv[0]);
    else
        result = 0u;
    if (kind == BIT_IOR || kind == BIT_XOR)
        result = 0u;
    if (kind == BIT_NOT) {
        if (!fixnum_arg(state, argv[0], &number)) return 0;
        result = ~(uint64_t)number;
    } else {
        for (i = 0u; i < argc; ++i) {
            if (!fixnum_arg(state, argv[i], &number)) return 0;
            if (kind == BIT_AND) result &= (uint64_t)number;
            else if (kind == BIT_IOR) result |= (uint64_t)number;
            else result ^= (uint64_t)number;
        }
    }
    return integer_value(state, signed_from_twos(result));
}

static Value primitive_logand(PState *s, size_t n, const Value *a)
{ return bit_operation(s, n, a, BIT_AND); }
static Value primitive_logior(PState *s, size_t n, const Value *a)
{ return bit_operation(s, n, a, BIT_IOR); }
static Value primitive_logxor(PState *s, size_t n, const Value *a)
{ return bit_operation(s, n, a, BIT_XOR); }
static Value primitive_lognot(PState *s, size_t n, const Value *a)
{ return bit_operation(s, n, a, BIT_NOT); }

static Value primitive_ash(PState *state, size_t argc, const Value *argv)
{
    int64_t value, shift;
    uint64_t bits;
    (void)argc;
    if (!fixnum_arg(state, argv[0], &value) ||
        !fixnum_arg(state, argv[1], &shift))
        return 0;
    if (shift >= 0) {
        /* A zero shifted left by any count is still zero. Avoid looping over
         * an arbitrarily large fixnum shift count for this common identity. */
        if (value == 0 || shift == 0)
            return integer_value(state, value);
        /* Pocket fixnums are narrower than int64_t, so any nonzero value
         * shifted left 64 or more places is necessarily out of range. */
        if (shift >= 64)
            return fail(state, P_ERR_RANGE, "ASH result is outside fixnum range");
        while (shift-- > 0) {
            if (value > p_fixnum_max() / 2 || value < p_fixnum_min() / 2)
                return fail(state, P_ERR_RANGE, "ASH result is outside fixnum range");
            value *= 2;
        }
    } else {
        uint64_t count = magnitude_i64(shift);
        if (count >= 64u) {
            value = value < 0 ? -1 : 0;
        } else if (count != 0u) {
            bits = (uint64_t)value >> count;
            if (value < 0)
                bits |= UINT64_MAX << (64u - (unsigned)count);
            value = signed_from_twos(bits);
        }
    }
    return integer_value(state, value);
}

static bool character_arg(PState *state, Value value, uint32_t *codepoint)
{
    if (!p_is_character(value)) {
        (void)p_error(state->p, P_ERR_TYPE, "expected a character");
        return false;
    }
    *codepoint = p_char_code(value);
    if (*codepoint > 0x10ffffu ||
        (*codepoint >= 0xd800u && *codepoint <= 0xdfffu)) {
        (void)p_error(state->p, P_ERR_RANGE, "character is not a Unicode scalar");
        return false;
    }
    return true;
}

static Value primitive_make_simple_vector(PState *state, size_t argc,
                                          const Value *argv)
{
    int64_t count;
    Value vector;
    (void)argc;
    if (!fixnum_arg(state, argv[0], &count))
        return 0;
    if (count < 0)
        return fail(state, P_ERR_RANGE, "simple-vector length is negative");
    vector = p_vector(state->p, (size_t)count, argv[1]);
    return vector ? one(state, vector) : fail(state, state->p->error.code,
                                              "simple-vector allocation failed");
}

static Value primitive_simple_vector_length(PState *state, size_t argc,
                                            const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_VECTOR)
        return fail(state, P_ERR_TYPE, "expected a simple vector");
    if (p_vector_ptr(argv[0])->length > (size_t)p_fixnum_max())
        return fail(state, P_ERR_RANGE, "simple-vector length exceeds fixnum range");
    return one(state, p_fixnum((int64_t)p_vector_ptr(argv[0])->length));
}

static Value primitive_svref(PState *state, size_t argc, const Value *argv)
{
    size_t index;
    (void)argc;
    if (p_type(argv[0]) != P_VECTOR)
        return fail(state, P_ERR_TYPE, "SVREF requires a simple vector");
    if (!index_arg(state, argv[1], p_vector_ptr(argv[0])->length, &index))
        return 0;
    return one(state, p_vector_ptr(argv[0])->data[index]);
}

static Value primitive_set_svref(PState *state, size_t argc, const Value *argv)
{
    size_t index;
    (void)argc;
    if (p_type(argv[0]) != P_VECTOR)
        return fail(state, P_ERR_TYPE, "SET-SVREF requires a simple vector");
    if (!index_arg(state, argv[1], p_vector_ptr(argv[0])->length, &index))
        return 0;
    p_vector_ptr(argv[0])->data[index] = argv[2];
    return one(state, argv[2]);
}

static Value primitive_make_string(PState *state, size_t argc,
                                   const Value *argv)
{
    int64_t count;
    uint32_t initial;
    size_t i;
    PString *string;
    (void)argc;
    if (!fixnum_arg(state, argv[0], &count) ||
        !character_arg(state, argv[1], &initial))
        return 0;
    if (count < 0 || (uint64_t)count > (SIZE_MAX - offsetof(PString, data)) /
                                         sizeof(uint32_t))
        return fail(state, P_ERR_RANGE, "string length is out of range");
    string = p_alloc(state->p, P_STRING,
                     offsetof(PString, data) + (size_t)count * sizeof(uint32_t));
    if (string == NULL)
        return fail(state, state->p->error.code, "string allocation failed");
    string->length = (size_t)count;
    for (i = 0; i < string->length; ++i)
        string->data[i] = initial;
    return one(state, (Value)string);
}

static Value primitive_string_length(PState *state, size_t argc,
                                     const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_STRING)
        return fail(state, P_ERR_TYPE, "expected a string");
    if (p_string_ptr(argv[0])->length > (size_t)p_fixnum_max())
        return fail(state, P_ERR_RANGE, "string length exceeds fixnum range");
    return one(state, p_fixnum((int64_t)p_string_ptr(argv[0])->length));
}

static Value primitive_schar(PState *state, size_t argc, const Value *argv)
{
    size_t index;
    (void)argc;
    if (p_type(argv[0]) != P_STRING)
        return fail(state, P_ERR_TYPE, "SCHAR requires a string");
    if (!index_arg(state, argv[1], p_string_ptr(argv[0])->length, &index))
        return 0;
    return one(state, p_character(p_string_ptr(argv[0])->data[index]));
}

static Value primitive_set_schar(PState *state, size_t argc, const Value *argv)
{
    size_t index;
    uint32_t codepoint;
    (void)argc;
    if (p_type(argv[0]) != P_STRING)
        return fail(state, P_ERR_TYPE, "SET-SCHAR requires a string");
    if (!index_arg(state, argv[1], p_string_ptr(argv[0])->length, &index) ||
        !character_arg(state, argv[2], &codepoint))
        return 0;
    p_string_ptr(argv[0])->data[index] = codepoint;
    return one(state, argv[2]);
}

static Value primitive_char_code(PState *state, size_t argc, const Value *argv)
{
    uint32_t codepoint;
    (void)argc;
    if (!character_arg(state, argv[0], &codepoint))
        return 0;
    return one(state, p_fixnum((int64_t)codepoint));
}

static Value primitive_code_char(PState *state, size_t argc, const Value *argv)
{
    int64_t codepoint;
    (void)argc;
    if (!fixnum_arg(state, argv[0], &codepoint))
        return 0;
    if (codepoint < 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return one(state, state->p->nil);
    return one(state, p_character((uint32_t)codepoint));
}

static Value primitive_make_bytes(PState *state, size_t argc,
                                  const Value *argv)
{
    int64_t count;
    Value bytes;
    (void)argc;
    if (!fixnum_arg(state, argv[0], &count))
        return 0;
    if (count < 0)
        return fail(state, P_ERR_RANGE, "byte-vector length is negative");
    bytes = p_bytes(state->p, (size_t)count);
    return bytes ? one(state, bytes) : fail(state, state->p->error.code,
                                             "byte-vector allocation failed");
}

static Value primitive_bytes_length(PState *state, size_t argc,
                                    const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_BYTES)
        return fail(state, P_ERR_TYPE, "expected a byte vector");
    if (((PBytes *)argv[0])->length > (size_t)p_fixnum_max())
        return fail(state, P_ERR_RANGE, "byte-vector length exceeds fixnum range");
    return one(state, p_fixnum((int64_t)((PBytes *)argv[0])->length));
}

static Value primitive_u8_ref(PState *state, size_t argc, const Value *argv)
{
    size_t index;
    (void)argc;
    if (p_type(argv[0]) != P_BYTES)
        return fail(state, P_ERR_TYPE, "U8-REF requires a byte vector");
    if (!index_arg(state, argv[1], ((PBytes *)argv[0])->length, &index))
        return 0;
    return one(state, p_fixnum(((PBytes *)argv[0])->data[index]));
}

static Value primitive_u8_set(PState *state, size_t argc, const Value *argv)
{
    int64_t octet;
    size_t index;
    (void)argc;
    if (p_type(argv[0]) != P_BYTES)
        return fail(state, P_ERR_TYPE, "U8-SET requires a byte vector");
    if (!index_arg(state, argv[1], ((PBytes *)argv[0])->length, &index) ||
        !fixnum_arg(state, argv[2], &octet))
        return 0;
    if (octet < 0 || octet > 255)
        return fail(state, P_ERR_RANGE, "byte value is outside 0..255");
    ((PBytes *)argv[0])->data[index] = (uint8_t)octet;
    return one(state, argv[2]);
}

static Value primitive_make_record(PState *state, size_t argc,
                                   const Value *argv)
{
    int64_t count;
    size_t i;
    Value record;
    (void)argc;
    if (!fixnum_arg(state, argv[1], &count))
        return 0;
    if (count < 0)
        return fail(state, P_ERR_RANGE, "record length is negative");
    record = p_record(state->p, argv[0], (size_t)count);
    if (!record)
        return fail(state, state->p->error.code, "record allocation failed");
    for (i = 0; i < (size_t)count; ++i)
        ((PRecord *)record)->data[i] = argc > 2u ? argv[2] : state->p->nil;
    return one(state, record);
}

static Value primitive_record_ref(PState *state, size_t argc,
                                  const Value *argv)
{
    size_t index;
    (void)argc;
    if (p_type(argv[0]) != P_RECORD)
        return fail(state, P_ERR_TYPE, "RECORD-REF requires a record");
    if (!index_arg(state, argv[1], ((PRecord *)argv[0])->length, &index))
        return 0;
    return one(state, ((PRecord *)argv[0])->data[index]);
}

static Value primitive_set_record_ref(PState *state, size_t argc,
                                      const Value *argv)
{
    size_t index;
    (void)argc;
    if (p_type(argv[0]) != P_RECORD)
        return fail(state, P_ERR_TYPE, "SET-RECORD-REF requires a record");
    if (!index_arg(state, argv[1], ((PRecord *)argv[0])->length, &index))
        return 0;
    ((PRecord *)argv[0])->data[index] = argv[2];
    return one(state, argv[2]);
}

static bool name_to_utf8(PState *state, Value name, char out[PRIMITIVE_NAME_CAPACITY])
{
    size_t written = 0u, i;
    if (p_type(name) != P_STRING) {
        (void)p_error(state->p, P_ERR_TYPE, "expected a string name");
        return false;
    }
    for (i = 0u; i < p_string_ptr(name)->length; ++i) {
        if (p_string_ptr(name)->data[i] == 0u) {
            (void)p_error(state->p, P_ERR_RANGE,
                          "symbol and package names cannot contain NUL");
            return false;
        }
    }
    if (!p_string_to_utf8(state->p, name, out, PRIMITIVE_NAME_CAPACITY, &written))
        return false;
    out[written] = '\0';
    return true;
}

static bool same_string(Value left, Value right)
{
    size_t i;
    if (p_type(left) != P_STRING || p_type(right) != P_STRING)
        return false;
    if (p_string_ptr(left)->length != p_string_ptr(right)->length)
        return false;
    for (i = 0u; i < p_string_ptr(left)->length; ++i) {
        if (p_string_ptr(left)->data[i] != p_string_ptr(right)->data[i])
            return false;
    }
    return true;
}

static Value package_designator(PState *state, Value designator)
{
    Value name;
    char text[PRIMITIVE_NAME_CAPACITY];
    if (p_type(designator) == P_PACKAGE)
        return designator;
    if (p_type(designator) == P_SYMBOL)
        name = p_symbol_ptr(designator)->name;
    else if (p_type(designator) == P_STRING)
        name = designator;
    else {
        (void)p_error(state->p, P_ERR_TYPE, "expected a package designator");
        return 0;
    }
    if (!name_to_utf8(state, name, text))
        return 0;
    return p_find_package(state->p, text);
}

static Value current_package_or_error(PState *state, Value supplied)
{
    Value package = supplied == P_UNSUPPLIED
                  ? state->p->current_package
                  : package_designator(state, supplied);
    if (!package && state->p->error.code == P_OK)
        return fail(state, P_ERR_PACKAGE, "package does not exist");
    if (!package)
        return 0u;
    if (p_type(package) != P_PACKAGE)
        return fail(state, P_ERR_TYPE, "expected a package");
    return package;
}

static Value primitive_make_symbol(PState *state, size_t argc,
                                  const Value *argv)
{
    char name[PRIMITIVE_NAME_CAPACITY];
    Value symbol;
    (void)argc;
    if (!name_to_utf8(state, argv[0], name))
        return 0;
    symbol = p_make_symbol(state->p, name);
    return symbol ? one(state, symbol) : fail(state, state->p->error.code,
                                               "symbol allocation failed");
}

static Value primitive_symbol_name(PState *state, size_t argc,
                                   const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "SYMBOL-NAME requires a symbol");
    return one(state, p_symbol_ptr(argv[0])->name);
}

static Value primitive_symbol_package(PState *state, size_t argc,
                                      const Value *argv)
{
    Value home;
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "SYMBOL-PACKAGE requires a symbol");
    home = p_symbol_ptr(argv[0])->home;
    return one(state, home == state->p->nil ? state->p->nil : home);
}

static Value primitive_symbol_value(PState *state, size_t argc,
                                    const Value *argv)
{
    Value value;
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "SYMBOL-VALUE requires a symbol");
    value = p_symbol_ptr(argv[0])->value;
    if (value == P_UNBOUND)
        return fail(state, P_ERR_UNBOUND, "symbol has no value");
    return one(state, value);
}

static Value primitive_set_symbol_value(PState *state, size_t argc,
                                         const Value *argv)
{
    PSymbol *symbol;
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "SET-SYMBOL-VALUE requires a symbol");
    symbol = p_symbol_ptr(argv[0]);
    if ((symbol->flags & P_CONSTANT) != 0u && symbol->value != argv[1])
        return fail(state, P_ERR_UNSUPPORTED, "cannot change a constant symbol");
    symbol->value = argv[1];
    return one(state, argv[1]);
}

static Value primitive_symbol_function(PState *state, size_t argc,
                                       const Value *argv)
{
    Value function;
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "SYMBOL-FUNCTION requires a symbol");
    function = p_symbol_ptr(argv[0])->function;
    if (function == P_UNBOUND)
        return fail(state, P_ERR_UNDEFINED, "symbol has no function definition");
    return one(state, function);
}

static Value primitive_set_symbol_function(PState *state, size_t argc,
                                           const Value *argv)
{
    PSymbol *symbol;
    PType type;
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "SET-SYMBOL-FUNCTION requires a symbol");
    type = p_type(argv[1]);
    if (type != P_CLOSURE && type != P_PRIMITIVE)
        return fail(state, P_ERR_TYPE, "function cell requires a function");
    symbol = p_symbol_ptr(argv[0]);
    symbol->function = argv[1];
    symbol->flags &= ~P_MACRO;
    return one(state, argv[1]);
}

static Value primitive_symbol_plist(PState *state, size_t argc,
                                    const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "SYMBOL-PLIST requires a symbol");
    return one(state, p_symbol_ptr(argv[0])->plist);
}

static Value primitive_set_symbol_plist(PState *state, size_t argc,
                                        const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "SET-SYMBOL-PLIST requires a symbol");
    p_symbol_ptr(argv[0])->plist = argv[1];
    return one(state, argv[1]);
}

static Value primitive_boundp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "BOUNDP requires a symbol");
    return one(state, p_symbol_ptr(argv[0])->value == P_UNBOUND
                      ? state->p->nil : state->p->truth);
}

static Value primitive_fboundp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "FBOUNDP requires a symbol");
    return one(state, p_symbol_ptr(argv[0])->function == P_UNBOUND
                      ? state->p->nil : state->p->truth);
}

static Value primitive_keywordp(PState *state, size_t argc, const Value *argv)
{
    (void)argc;
    return one(state, p_type(argv[0]) == P_SYMBOL &&
                      p_symbol_ptr(argv[0])->home == state->p->keyword
                      ? state->p->truth : state->p->nil);
}

static Value primitive_find_package(PState *state, size_t argc,
                                    const Value *argv)
{
    Value package = package_designator(state, argv[0]);
    (void)argc;
    if (!package && state->p->error.code == P_OK)
        return one(state, state->p->nil);
    if (!package)
        return 0;
    return one(state, package);
}

static Value primitive_package_name(PState *state, size_t argc,
                                    const Value *argv)
{
    (void)argc;
    if (p_type(argv[0]) != P_PACKAGE)
        return fail(state, P_ERR_TYPE, "PACKAGE-NAME requires a package");
    return one(state, ((PPackage *)argv[0])->name);
}

static Value symbol_status(PState *state, int status)
{
    static const char *const names[] = { NULL, "INTERNAL", "EXTERNAL", "INHERITED" };
    Value symbol;
    if (status <= 0)
        return state->p->nil;
    symbol = p_intern(state->p, state->p->keyword, names[status], NULL);
    return symbol;
}

static Value multiple_two(PState *state, Value first, Value second)
{
    state->mv[0] = first;
    state->mv[1] = second;
    state->mv_count = 2u;
    return first;
}

static Value primitive_intern(PState *state, size_t argc, const Value *argv)
{
    Value package = current_package_or_error(state,
                    argc > 1u ? argv[1] : P_UNSUPPLIED);
    Value symbol, status_value;
    char name[PRIMITIVE_NAME_CAPACITY];
    int status = 0;
    if (!package || !name_to_utf8(state, argv[0], name))
        return 0;
    symbol = p_intern(state->p, package, name, &status);
    if (!symbol)
        return fail(state, state->p->error.code, "INTERN failed");
    status_value = symbol_status(state, status);
    if (!status_value)
        return fail(state, state->p->error.code, "unable to create INTERN status");
    return multiple_two(state, symbol, status_value);
}

static Value primitive_find_symbol(PState *state, size_t argc,
                                   const Value *argv)
{
    Value package = current_package_or_error(state,
                    argc > 1u ? argv[1] : P_UNSUPPLIED);
    Value symbol, status_value;
    char name[PRIMITIVE_NAME_CAPACITY];
    int status = 0;
    if (!package || !name_to_utf8(state, argv[0], name))
        return 0;
    symbol = p_find_symbol(state->p, package, name, &status);
    if (!symbol)
        return 0;
    status_value = symbol_status(state, status);
    if (!status_value)
        return fail(state, state->p->error.code, "unable to create FIND-SYMBOL status");
    return multiple_two(state, symbol, status_value);
}

static Value primitive_make_package(PState *state, size_t argc,
                                    const Value *argv)
{
    char name[PRIMITIVE_NAME_CAPACITY];
    Value package;
    (void)argc;
    if (!name_to_utf8(state, argv[0], name))
        return 0;
    package = p_make_package(state->p, name);
    return package ? one(state, package) : fail(state, state->p->error.code,
                                                 "unable to create package");
}

static Value primitive_in_package(PState *state, size_t argc,
                                  const Value *argv)
{
    Value package = package_designator(state, argv[0]);
    int status = 0;
    Value package_symbol;
    (void)argc;
    if (!package && state->p->error.code == P_OK)
        return fail(state, P_ERR_PACKAGE, "package does not exist");
    if (!package)
        return 0u;
    state->p->current_package = package;
    package_symbol = p_intern(state->p, state->p->cl, "*PACKAGE*", &status);
    if (!package_symbol)
        return fail(state, state->p->error.code, "unable to access *PACKAGE*");
    p_symbol_ptr(package_symbol)->flags |= P_SPECIAL;
    p_symbol_ptr(package_symbol)->value = package;
    return one(state, package);
}

static Value primitive_export(PState *state, size_t argc, const Value *argv)
{
    Value package = current_package_or_error(state,
                    argc > 1u ? argv[1] : P_UNSUPPLIED);
    if (!package)
        return 0;
    if (!p_export(state->p, package, argv[0]))
        return fail(state, state->p->error.code, "EXPORT failed");
    return one(state, state->p->truth);
}

static Value primitive_unexport(PState *state, size_t argc, const Value *argv)
{
    Value package = current_package_or_error(state,
                    argc > 1u ? argv[1] : P_UNSUPPLIED);
    if (!package)
        return 0;
    if (!p_unexport(state->p, package, argv[0]))
        return fail(state, state->p->error.code, "UNEXPORT failed");
    return one(state, state->p->truth);
}

static Value primitive_use_package(PState *state, size_t argc,
                                   const Value *argv)
{
    Value used = package_designator(state, argv[0]);
    Value package = current_package_or_error(state,
                    argc > 1u ? argv[1] : P_UNSUPPLIED);
    if ((!used || !package) && state->p->error.code == P_OK)
        return fail(state, P_ERR_PACKAGE, "package does not exist");
    if (!used || !package)
        return 0u;
    if (!p_use_package(state->p, package, used))
        return fail(state, state->p->error.code, "USE-PACKAGE failed");
    return one(state, state->p->truth);
}

static Value primitive_unuse_package(PState *state, size_t argc,
                                     const Value *argv)
{
    Value used = package_designator(state, argv[0]);
    Value package = current_package_or_error(state,
                    argc > 1u ? argv[1] : P_UNSUPPLIED);
    if ((!used || !package) && state->p->error.code == P_OK)
        return fail(state, P_ERR_PACKAGE, "package does not exist");
    if (!used || !package)
        return 0u;
    if (!p_unuse_package(state->p, package, used))
        return fail(state, state->p->error.code, "UNUSE-PACKAGE failed");
    return one(state, state->p->truth);
}

static Value primitive_import(PState *state, size_t argc, const Value *argv)
{
    Value package = current_package_or_error(state,
                    argc > 1u ? argv[1] : P_UNSUPPLIED);
    Value symbols = argv[0];
    size_t steps = 0u, max_steps = state->p->heap_size / 16u;
    if (!package)
        return 0;
    if (p_type(symbols) == P_SYMBOL) {
        if (!p_import(state->p, package, symbols, false))
            return fail(state, state->p->error.code, "IMPORT failed");
    } else {
        while (p_type(symbols) == P_CONS && steps++ < max_steps) {
            PCons *cell = p_cons_ptr(symbols);
            if (p_type(cell->car) != P_SYMBOL)
                return fail(state, P_ERR_TYPE, "IMPORT list contains a non-symbol");
            if (!p_import(state->p, package, cell->car, false))
                return fail(state, state->p->error.code, "IMPORT failed");
            symbols = cell->cdr;
        }
        if (symbols != state->p->nil)
            return fail(state, P_ERR_TYPE, "IMPORT requires a proper list of symbols");
    }
    return one(state, state->p->truth);
}

static Value primitive_shadow(PState *state, size_t argc, const Value *argv)
{
    Value package = current_package_or_error(state,
                    argc > 1u ? argv[1] : P_UNSUPPLIED);
    char name[PRIMITIVE_NAME_CAPACITY];
    if (!package || !name_to_utf8(state, argv[0], name))
        return 0;
    if (!p_shadow(state->p, package, name))
        return fail(state, state->p->error.code, "SHADOW failed");
    return one(state, state->p->truth);
}

static Value primitive_set_nicknames(PState *state, size_t argc,
                                     const Value *argv)
{
    Value package = package_designator(state, argv[0]);
    Value names = argv[1], cursor = names;
    size_t steps = 0u, max_steps = state->p->heap_size / 16u;
    if (!package && state->p->error.code == P_OK)
        return fail(state, P_ERR_PACKAGE, "package does not exist");
    if (!package)
        return 0u;
    while (p_type(cursor) == P_CONS && steps++ < max_steps) {
        char spelling[PRIMITIVE_NAME_CAPACITY];
        Value candidate = p_cons_ptr(cursor)->car;
        Value prior = names;
        size_t prior_count = 0u;
        if (!name_to_utf8(state, candidate, spelling))
            return 0;
        {
            Value existing = p_find_package(state->p, spelling);
            if (existing != 0u && existing != package)
                return fail(state, P_ERR_PACKAGE,
                            "package nickname is already in use");
        }
        while (prior != cursor && p_type(prior) == P_CONS &&
               prior_count++ < max_steps) {
            Value earlier = p_cons_ptr(prior)->car;
            if (same_string(earlier, candidate))
                return fail(state, P_ERR_PACKAGE,
                            "package nickname is duplicated");
            prior = p_cons_ptr(prior)->cdr;
        }
        if (p_type(candidate) != P_STRING)
            return fail(state, P_ERR_TYPE, "package nickname must be a string");
        cursor = p_cons_ptr(cursor)->cdr;
    }
    if (cursor != state->p->nil)
        return fail(state, P_ERR_TYPE, "package nicknames must be a proper list");
    ((PPackage *)package)->nicknames = names;
    (void)argc;
    return one(state, state->p->truth);
}

static Value primitive_defun(PState *state, size_t argc, const Value *argv)
{
    PSymbol *symbol;
    PType type;
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "%DEFUN requires a symbol name");
    type = p_type(argv[1]);
    if (type != P_CLOSURE && type != P_PRIMITIVE)
        return fail(state, P_ERR_TYPE, "%DEFUN requires a function value");
    symbol = p_symbol_ptr(argv[0]);
    symbol->function = argv[1];
    symbol->flags &= ~P_MACRO;
    if (type == P_CLOSURE)
        ((PClosure *)argv[1])->name = argv[0];
    if (symbol->home == state->p->cl &&
        !p_export(state->p, state->p->cl, argv[0]))
        return fail(state, state->p->error.code, "unable to export CL function");
    return one(state, argv[0]);
}

static Value primitive_defmacro(PState *state, size_t argc, const Value *argv)
{
    PSymbol *symbol;
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL || p_type(argv[1]) != P_CLOSURE)
        return fail(state, P_ERR_TYPE, "%DEFMACRO requires a symbol and closure");
    symbol = p_symbol_ptr(argv[0]);
    symbol->function = argv[1];
    symbol->flags |= P_MACRO;
    ((PClosure *)argv[1])->name = argv[0];
    if (symbol->home == state->p->cl &&
        !p_export(state->p, state->p->cl, argv[0]))
        return fail(state, state->p->error.code, "unable to export CL macro");
    return one(state, argv[0]);
}

static Value primitive_proclaim_special(PState *state, size_t argc,
                                        const Value *argv)
{
    Value cursor;
    size_t steps = 0u, max_steps = state->p->heap_size / 16u;
    (void)argc;
    if (p_type(argv[0]) == P_SYMBOL) {
        p_symbol_ptr(argv[0])->flags |= P_SPECIAL;
        return one(state, argv[0]);
    }
    cursor = argv[0];
    while (p_type(cursor) == P_CONS && steps++ < max_steps) {
        Value symbol = p_cons_ptr(cursor)->car;
        if (p_type(symbol) != P_SYMBOL)
            return fail(state, P_ERR_TYPE, "%PROCLAIM-SPECIAL requires symbols");
        p_symbol_ptr(symbol)->flags |= P_SPECIAL;
        cursor = p_cons_ptr(cursor)->cdr;
    }
    if (cursor != state->p->nil)
        return fail(state, P_ERR_TYPE, "%PROCLAIM-SPECIAL requires a proper symbol list");
    return one(state, state->p->nil);
}

static Value primitive_define_constant(PState *state, size_t argc,
                                       const Value *argv)
{
    PSymbol *symbol;
    (void)argc;
    if (p_type(argv[0]) != P_SYMBOL)
        return fail(state, P_ERR_TYPE, "%DEFINE-CONSTANT requires a symbol");
    symbol = p_symbol_ptr(argv[0]);
    if ((symbol->flags & P_CONSTANT) != 0u && symbol->value != argv[1])
        return fail(state, P_ERR_UNSUPPORTED, "constant value cannot be changed");
    symbol->value = argv[1];
    symbol->flags |= P_CONSTANT | P_SPECIAL;
    return one(state, argv[0]);
}

static Value primitive_macro_function(PState *state, size_t argc,
                                      const Value *argv)
{
    Value environment = argc > 1u ? argv[1] : state->current_env;
    Value result;
    result = p_macro_function(state, environment, argv[0]);
    if (result == 0u && state->p->error.code != P_OK)
        return 0;
    return one(state, result == 0u ? state->p->nil : result);
}

static Value primitive_error(PState *state, size_t argc, const Value *argv)
{
    char message[256];
    size_t written = 0u;
    (void)argc;
    if (p_type(argv[0]) != P_STRING)
        return fail(state, P_ERR_TYPE, "%ERROR requires a string message");
    if (!p_string_to_utf8(state->p, argv[0], message, sizeof(message) - 1u, &written))
        return 0;
    message[written] = '\0';
    return fail(state, P_ERR_CONTROL, message);
}

/* FUNCALL and APPLY are dispatched by eval0 itself so their targets can
 * return multiple values without re-entering the evaluator from C. */
static Value primitive_control_placeholder(PState *state, size_t argc,
                                           const Value *argv)
{
    (void)argc; (void)argv;
    return fail(state, P_ERR_UNSUPPORTED,
                "control primitive bypassed evaluator dispatch");
}

static Value primitive_abs(PState *state, size_t argc, const Value *argv)
{
    double number;
    bool integer;
    int64_t value;
    (void)argc;
    if (!numeric_arg(state, argv[0], &number, &integer))
        return 0;
    if (!integer)
        return floating_value(state, fabs(number));
    value = p_fixnum_value(argv[0]);
    if (value == p_fixnum_min())
        return fail(state, P_ERR_RANGE, "ABS result is outside fixnum range");
    return integer_value(state, value < 0 ? -value : value);
}

static Value primitive_one_plus(PState *state, size_t argc,
                                const Value *argv)
{
    double number;
    bool integer;
    int64_t next;
    (void)argc;
    if (!numeric_arg(state, argv[0], &number, &integer))
        return 0;
    if (!integer)
        return floating_value(state, number + 1.0);
    if (!checked_add(p_fixnum_value(argv[0]), 1, &next))
        return fail(state, P_ERR_RANGE, "1+ result is outside fixnum range");
    return integer_value(state, next);
}

static Value primitive_one_minus(PState *state, size_t argc,
                                 const Value *argv)
{
    double number;
    bool integer;
    int64_t next;
    (void)argc;
    if (!numeric_arg(state, argv[0], &number, &integer))
        return 0;
    if (!integer)
        return floating_value(state, number - 1.0);
    if (!checked_subtract(p_fixnum_value(argv[0]), 1, &next))
        return fail(state, P_ERR_RANGE, "1- result is outside fixnum range");
    return integer_value(state, next);
}

typedef enum { MATH_SQRT, MATH_SIN, MATH_COS, MATH_TAN, MATH_EXP, MATH_LOG } MathKind;

static Value primitive_math(PState *state, size_t argc, const Value *argv,
                            MathKind kind)
{
    double x, result;
    bool integer;
    (void)argc;
    if (!numeric_arg(state, argv[0], &x, &integer))
        return 0;
    switch (kind) {
    case MATH_SQRT:
        if (x < 0.0) return fail(state, P_ERR_RANGE, "SQRT domain error");
        result = sqrt(x); break;
    case MATH_SIN: result = sin(x); break;
    case MATH_COS: result = cos(x); break;
    case MATH_TAN: result = tan(x); break;
    case MATH_EXP: result = exp(x); break;
    default:
        if (x <= 0.0) return fail(state, P_ERR_RANGE, "LOG domain error");
        result = log(x); break;
    }
    if (!isfinite(result))
        return fail(state, P_ERR_RANGE, "floating-point result is not finite");
    return floating_value(state, result);
}

static Value primitive_sqrt(PState *s, size_t n, const Value *a) { return primitive_math(s, n, a, MATH_SQRT); }
static Value primitive_sin(PState *s, size_t n, const Value *a) { return primitive_math(s, n, a, MATH_SIN); }
static Value primitive_cos(PState *s, size_t n, const Value *a) { return primitive_math(s, n, a, MATH_COS); }
static Value primitive_tan(PState *s, size_t n, const Value *a) { return primitive_math(s, n, a, MATH_TAN); }
static Value primitive_exp(PState *s, size_t n, const Value *a) { return primitive_math(s, n, a, MATH_EXP); }
static Value primitive_log(PState *s, size_t n, const Value *a) { return primitive_math(s, n, a, MATH_LOG); }

static Value primitive_atan(PState *state, size_t argc, const Value *argv)
{
    double y, x, result;
    bool integer;
    if (!numeric_arg(state, argv[0], &y, &integer))
        return 0u;
    if (argc == 1u) {
        result = atan(y);
    } else {
        if (!numeric_arg(state, argv[1], &x, &integer))
            return 0u;
        result = atan2(y, x);
    }
    if (!isfinite(result))
        return fail(state, P_ERR_RANGE, "ATAN result is not finite");
    return floating_value(state, result);
}

static Value primitive_expt(PState *state, size_t argc, const Value *argv)
{
    double base, power;
    bool base_integer, power_integer;
    double result;
    (void)argc;
    if (!numeric_arg(state, argv[0], &base, &base_integer) ||
        !numeric_arg(state, argv[1], &power, &power_integer))
        return 0;
    if (base_integer && power_integer) {
        int64_t b = p_fixnum_value(argv[0]);
        int64_t e = p_fixnum_value(argv[1]);
        int64_t product = 1;
        uint64_t remaining;
        if (e < 0) {
            if (b == 0)
                return fail(state, P_ERR_RANGE, "zero cannot have a negative exponent");
            if (b == 1)
                return integer_value(state, 1);
            if (b == -1)
                return integer_value(state, e % 2 == 0 ? 1 : -1);
            return fail(state, P_ERR_UNSUPPORTED,
                        "negative integer EXPT requires ratios");
        }
        remaining = (uint64_t)e;
        while (remaining != 0u) {
            int64_t squared;
            if ((remaining & 1u) != 0u &&
                !checked_multiply(product, b, &product))
                return fail(state, P_ERR_RANGE, "integer EXPT result is out of range");
            remaining >>= 1u;
            if (remaining != 0u && !checked_multiply(b, b, &squared))
                return fail(state, P_ERR_RANGE, "integer EXPT result is out of range");
            if (remaining != 0u)
                b = squared;
        }
        return integer_value(state, product);
    }
    result = pow(base, power);
    if (!isfinite(result))
        return fail(state, P_ERR_RANGE, "EXPT result is not finite");
    return floating_value(state, result);
}

static Value primitive_read_file(PState *state, size_t argc,
                                 const Value *argv)
{
    size_t length;
    Value bytes;
    (void)argc;
    if (!p_host_file_size(state->p, argv[0], &length))
        return 0;
    bytes = p_bytes(state->p, length);
    if (!bytes)
        return fail(state, state->p->error.code, "unable to allocate file bytes");
    if (!p_host_read_file(state->p, argv[0], bytes))
        return 0;
    return one(state, bytes);
}

static Value primitive_write_file(PState *state, size_t argc,
                                  const Value *argv)
{
    (void)argc;
    if (!p_host_write_file(state->p, argv[0], argv[1]))
        return 0;
    return one(state, state->p->truth);
}

static Value primitive_console_write(PState *state, size_t argc,
                                     const Value *argv)
{
    (void)argc;
    if (!p_host_console_write_string(state->p, argv[0]))
        return 0;
    return one(state, state->p->nil);
}

static Value primitive_console_read_line(PState *state, size_t argc,
                                         const Value *argv)
{
    char line[CONSOLE_LINE_CAPACITY];
    size_t length = 0u;
    bool eof = false;
    Value result;
    (void)argc; (void)argv;
    if (!p_host_console_read_line(state->p, line, sizeof(line), &length, &eof))
        return 0;
    if (eof)
        return one(state, state->p->nil);
    result = p_string_utf8(state->p, line, length);
    return result ? one(state, result) : fail(state, state->p->error.code,
                                               "unable to allocate console line");
}

static bool reserve_package_mutation(PState *state, size_t argc,
                                     const Value *argv, size_t *bytes)
{
    Value cursor = state->p->packages;
    size_t packages = 0u, limit = state->p->heap_size / 16u;
    (void)argc; (void)argv;
    while (p_type(cursor) == P_PACKAGE && packages < limit) {
        ++packages;
        cursor = ((PPackage *)cursor)->next;
    }
    if (packages > (SIZE_MAX - 64u) / sizeof(PPackageEntry)) {
        (void)p_error(state->p, P_ERR_RANGE, "too many packages");
        return false;
    }
    /* Export may need one fresh package entry in every using package. */
    *bytes = packages * sizeof(PPackageEntry) + sizeof(PCons) + 64u;
    return true;
}

static bool reserve_import(PState *state, size_t argc, const Value *argv,
                           size_t *bytes)
{
    Value cursor;
    size_t count = 0u, limit = state->p->heap_size / 16u;
    (void)argc;
    if (p_type(argv[0]) == P_SYMBOL) {
        count = 1u;
    } else {
        cursor = argv[0];
        while (p_type(cursor) == P_CONS && count < limit) {
            if (p_type(p_cons_ptr(cursor)->car) != P_SYMBOL) {
                (void)p_error(state->p, P_ERR_TYPE,
                              "IMPORT list contains a non-symbol");
                return false;
            }
            ++count;
            cursor = p_cons_ptr(cursor)->cdr;
        }
        if (cursor != state->p->nil) {
            (void)p_error(state->p, P_ERR_TYPE,
                          "IMPORT requires a finite proper list");
            return false;
        }
    }
    if (count > (SIZE_MAX - 64u) / (sizeof(PPackageEntry) + sizeof(PCons))) {
        (void)p_error(state->p, P_ERR_RANGE, "too many symbols to import");
        return false;
    }
    *bytes = count * (sizeof(PPackageEntry) + sizeof(PCons)) + 64u;
    return true;
}

static bool reserve_symbol_creation(PState *state, size_t argc,
                                    const Value *argv, size_t *bytes)
{
    return reserve_intern(state, argc, argv, bytes);
}

typedef struct {
    const char *name;
    PPrimitiveFn entry;
    PPrimitiveReserveFn reserve;
    size_t min_arity, max_arity;
    unsigned flags;
    bool standard;
} PrimitiveSpec;

#define SPEC(name, fn, min, max, reserve, standard) \
    { name, fn, reserve, min, max, 0u, standard }
#define SPEC_CONTROL(name, min, max, flags) \
    { name, primitive_control_placeholder, NULL, min, max, flags, true }
#define VARIADIC P_EVAL_PRIMITIVE_VARIADIC

bool p_is_builtin_primitive(Pocket *p, Value symbol, const char *name)
{
    PPrimitiveFn expected = NULL;
    (void)p;
    if (p_type(symbol) != P_SYMBOL ||
        p_type(p_symbol_ptr(symbol)->function) != P_PRIMITIVE) return false;
    if (strcmp(name, "+") == 0) expected = primitive_add;
    else if (strcmp(name, "-") == 0) expected = primitive_subtract;
    else if (strcmp(name, "<") == 0) expected = primitive_num_lt;
    else if (strcmp(name, "=") == 0) expected = primitive_num_eq;
    else if (strcmp(name, "CONS") == 0) expected = primitive_cons;
    else if (strcmp(name, "CAR") == 0) expected = primitive_car;
    else if (strcmp(name, "CDR") == 0) expected = primitive_cdr;
    return expected && ((PPrimitive *)p_symbol_ptr(symbol)->function)->entry == expected;
}

static const PrimitiveSpec primitive_specs[] = {
    SPEC("CONS", primitive_cons, 2u, 2u, reserve_cons, true),
    SPEC("CAR", primitive_car, 1u, 1u, NULL, true),
    SPEC("CDR", primitive_cdr, 1u, 1u, NULL, true),
    SPEC("RPLACA", primitive_rplaca, 2u, 2u, NULL, true),
    SPEC("RPLACD", primitive_rplacd, 2u, 2u, NULL, true),
    SPEC("EQ", primitive_eq, 2u, 2u, NULL, true),
    SPEC("EQL", primitive_eql, 2u, 2u, NULL, true),
    SPEC("ATOM", primitive_atom, 1u, 1u, NULL, true),
    SPEC("CONSP", primitive_consp, 1u, 1u, NULL, true),
    SPEC("LISTP", primitive_listp, 1u, 1u, NULL, true),
    SPEC("NULL", primitive_null, 1u, 1u, NULL, true),
    SPEC("NOT", primitive_null, 1u, 1u, NULL, true),
    SPEC("SYMBOLP", primitive_symbolp, 1u, 1u, NULL, true),
    SPEC("NUMBERP", primitive_numberp, 1u, 1u, NULL, true),
    SPEC("INTEGERP", primitive_integerp, 1u, 1u, NULL, true),
    SPEC("FLOATP", primitive_floatp, 1u, 1u, NULL, true),
    SPEC("CHARACTERP", primitive_characterp, 1u, 1u, NULL, true),
    SPEC("STRINGP", primitive_stringp, 1u, 1u, NULL, true),
    SPEC("VECTORP", primitive_vectorp, 1u, 1u, NULL, true),
    SPEC("SIMPLE-VECTOR-P", primitive_simple_vectorp, 1u, 1u, NULL, true),
    SPEC("FUNCTIONP", primitive_functionp, 1u, 1u, NULL, true),
    SPEC("PACKAGEP", primitive_packagep, 1u, 1u, NULL, true),
    { "VALUES", primitive_values, NULL, 0u, VARIADIC,
      P_PRIMITIVE_EVAL_VALUES, true },
    SPEC_CONTROL("FUNCALL", 1u, VARIADIC, P_PRIMITIVE_EVAL_FUNCALL),
    SPEC_CONTROL("APPLY", 2u, VARIADIC, P_PRIMITIVE_EVAL_APPLY),
    SPEC("+", primitive_add, 0u, VARIADIC, reserve_flonum, true),
    SPEC("-", primitive_subtract, 1u, VARIADIC, reserve_flonum, true),
    SPEC("*", primitive_multiply, 0u, VARIADIC, reserve_flonum, true),
    SPEC("/", primitive_divide, 1u, VARIADIC, reserve_flonum, true),
    SPEC("=", primitive_num_eq, 2u, VARIADIC, NULL, true),
    SPEC("/=", primitive_num_ne, 2u, VARIADIC, NULL, true),
    SPEC("<", primitive_num_lt, 2u, VARIADIC, NULL, true),
    SPEC("<=", primitive_num_le, 2u, VARIADIC, NULL, true),
    SPEC(">", primitive_num_gt, 2u, VARIADIC, NULL, true),
    SPEC(">=", primitive_num_ge, 2u, VARIADIC, NULL, true),
    SPEC("TRUNCATE", primitive_truncate, 1u, 2u, reserve_flonum, true),
    SPEC("FLOOR", primitive_floor, 1u, 2u, reserve_flonum, true),
    SPEC("CEILING", primitive_ceiling, 1u, 2u, reserve_flonum, true),
    SPEC("ROUND", primitive_round, 1u, 2u, reserve_flonum, true),
    SPEC("REM", primitive_rem, 2u, 2u, reserve_flonum, true),
    SPEC("MOD", primitive_mod, 2u, 2u, reserve_flonum, true),
    SPEC("LOGAND", primitive_logand, 0u, VARIADIC, NULL, true),
    SPEC("LOGIOR", primitive_logior, 0u, VARIADIC, NULL, true),
    SPEC("LOGXOR", primitive_logxor, 0u, VARIADIC, NULL, true),
    SPEC("LOGNOT", primitive_lognot, 1u, 1u, NULL, true),
    SPEC("ASH", primitive_ash, 2u, 2u, NULL, true),
    SPEC("1+", primitive_one_plus, 1u, 1u, reserve_flonum, true),
    SPEC("1-", primitive_one_minus, 1u, 1u, reserve_flonum, true),
    SPEC("ABS", primitive_abs, 1u, 1u, reserve_flonum, true),
    SPEC("SQRT", primitive_sqrt, 1u, 1u, reserve_float_result, true),
    SPEC("SIN", primitive_sin, 1u, 1u, reserve_float_result, true),
    SPEC("COS", primitive_cos, 1u, 1u, reserve_float_result, true),
    SPEC("TAN", primitive_tan, 1u, 1u, reserve_float_result, true),
    SPEC("EXP", primitive_exp, 1u, 1u, reserve_float_result, true),
    SPEC("LOG", primitive_log, 1u, 1u, reserve_float_result, true),
    SPEC("ATAN", primitive_atan, 1u, 2u, reserve_float_result, true),
    SPEC("EXPT", primitive_expt, 2u, 2u, reserve_float_result, true),
    SPEC("MAKE-SYMBOL", primitive_make_symbol, 1u, 1u, reserve_symbol_creation, true),
    SPEC("SYMBOL-NAME", primitive_symbol_name, 1u, 1u, NULL, true),
    SPEC("SYMBOL-PACKAGE", primitive_symbol_package, 1u, 1u, NULL, true),
    SPEC("SYMBOL-VALUE", primitive_symbol_value, 1u, 1u, NULL, true),
    SPEC("SET", primitive_set_symbol_value, 2u, 2u, NULL, true),
    SPEC("SYMBOL-FUNCTION", primitive_symbol_function, 1u, 1u, NULL, true),
    SPEC("%SET-SYMBOL-FUNCTION", primitive_set_symbol_function, 2u, 2u, NULL, false),
    SPEC("SYMBOL-PLIST", primitive_symbol_plist, 1u, 1u, NULL, true),
    SPEC("%SET-SYMBOL-PLIST", primitive_set_symbol_plist, 2u, 2u, NULL, false),
    SPEC("BOUNDP", primitive_boundp, 1u, 1u, NULL, true),
    SPEC("FBOUNDP", primitive_fboundp, 1u, 1u, NULL, true),
    SPEC("KEYWORDP", primitive_keywordp, 1u, 1u, NULL, true),
    SPEC("FIND-PACKAGE", primitive_find_package, 1u, 1u, NULL, true),
    SPEC("PACKAGE-NAME", primitive_package_name, 1u, 1u, NULL, true),
    SPEC("INTERN", primitive_intern, 1u, 2u, reserve_intern, true),
    SPEC("FIND-SYMBOL", primitive_find_symbol, 1u, 2u, reserve_intern, true),
    SPEC("EXPORT", primitive_export, 1u, 2u, reserve_package_mutation, true),
    SPEC("UNEXPORT", primitive_unexport, 1u, 2u, NULL, true),
    SPEC("USE-PACKAGE", primitive_use_package, 1u, 2u, reserve_cons, true),
    SPEC("UNUSE-PACKAGE", primitive_unuse_package, 1u, 2u, NULL, true),
    SPEC("IMPORT", primitive_import, 1u, 2u, reserve_import, true),
    SPEC("SHADOW", primitive_shadow, 1u, 2u, reserve_symbol_creation, true),
    SPEC("MAKE-PACKAGE", primitive_make_package, 1u, 1u, reserve_package, true),
    SPEC("CHAR-CODE", primitive_char_code, 1u, 1u, NULL, true),
    SPEC("CODE-CHAR", primitive_code_char, 1u, 1u, NULL, true),
    SPEC("SCHAR", primitive_schar, 2u, 2u, NULL, true),
    SPEC("SVREF", primitive_svref, 2u, 2u, NULL, true),
    SPEC("%MAKE-SIMPLE-VECTOR", primitive_make_simple_vector, 2u, 2u, reserve_vector, false),
    SPEC("%SIMPLE-VECTOR-LENGTH", primitive_simple_vector_length, 1u, 1u, NULL, false),
    SPEC("%SET-SVREF", primitive_set_svref, 3u, 3u, NULL, false),
    SPEC("%MAKE-STRING", primitive_make_string, 2u, 2u, reserve_string, false),
    SPEC("%STRING-LENGTH", primitive_string_length, 1u, 1u, NULL, false),
    SPEC("%SET-SCHAR", primitive_set_schar, 3u, 3u, NULL, false),
    SPEC("%MAKE-BYTES", primitive_make_bytes, 1u, 1u, reserve_bytes_vector, false),
    SPEC("%BYTES-LENGTH", primitive_bytes_length, 1u, 1u, NULL, false),
    SPEC("%U8-REF", primitive_u8_ref, 2u, 2u, NULL, false),
    SPEC("%U8-SET", primitive_u8_set, 3u, 3u, NULL, false),
    SPEC("%MAKE-RECORD", primitive_make_record, 2u, 3u, reserve_record, false),
    SPEC("%RECORD-REF", primitive_record_ref, 2u, 2u, NULL, false),
    SPEC("%SET-RECORD-REF", primitive_set_record_ref, 3u, 3u, NULL, false),
    SPEC("%DEFUN", primitive_defun, 2u, 2u, NULL, false),
    SPEC("%DEFMACRO", primitive_defmacro, 2u, 2u, NULL, false),
    SPEC("%PROCLAIM-SPECIAL", primitive_proclaim_special, 1u, 1u, NULL, false),
    SPEC("%DEFINE-CONSTANT", primitive_define_constant, 2u, 2u, NULL, false),
    SPEC("%MACRO-FUNCTION", primitive_macro_function, 1u, 2u, NULL, false),
    SPEC("%IN-PACKAGE", primitive_in_package, 1u, 1u, reserve_package_mutation, false),
    SPEC("%MAKE-PACKAGE", primitive_make_package, 1u, 1u, reserve_package, false),
    SPEC("%EXPORT", primitive_export, 2u, 2u, reserve_package_mutation, false),
    SPEC("%UNEXPORT", primitive_unexport, 2u, 2u, NULL, false),
    SPEC("%USE-PACKAGE", primitive_use_package, 2u, 2u, reserve_cons, false),
    SPEC("%UNUSE-PACKAGE", primitive_unuse_package, 2u, 2u, NULL, false),
    SPEC("%IMPORT", primitive_import, 2u, 2u, reserve_import, false),
    SPEC("%SHADOW", primitive_shadow, 2u, 2u, reserve_symbol_creation, false),
    SPEC("%SET-NICKNAMES", primitive_set_nicknames, 2u, 2u, NULL, false),
    SPEC("%READ-FILE", primitive_read_file, 1u, 1u, reserve_host_read, false),
    SPEC("%WRITE-FILE", primitive_write_file, 2u, 2u, NULL, false),
    SPEC("%CONSOLE-WRITE-STRING", primitive_console_write, 1u, 1u, NULL, false),
    SPEC("%CONSOLE-READ-LINE", primitive_console_read_line, 0u, 0u, reserve_console_line, false),
    SPEC("%ERROR", primitive_error, 1u, 1u, NULL, false)
};

static Value intern_primitive_name(Pocket *p, Value package,
                                   const char *name, bool standard)
{
    Value symbol = p_intern(p, package, name, NULL);
    if (!symbol)
        return 0;
    if (standard && !p_export(p, package, symbol))
        return 0;
    return symbol;
}

static bool install_special_variable(Pocket *p, const char *name, Value value)
{
    Value symbol = intern_primitive_name(p, p->cl, name, true);
    if (!symbol)
        return false;
    p_symbol_ptr(symbol)->flags |= P_SPECIAL;
    p_symbol_ptr(symbol)->value = value;
    return true;
}

static bool add_primitive_reserved(PState *state, const char *name,
                                   bool standard, PPrimitiveFn entry,
                                   PPrimitiveReserveFn reserve,
                                   size_t min_arity, size_t max_arity,
                                   unsigned flags)
{
    Pocket *p = state->p;
    Value package = standard ? p->cl : p->sys;
    Value symbol = intern_primitive_name(p, package, name, standard);
    if (symbol == 0u) return false;
    PPrimitive *primitive = (PPrimitive *)p_alloc(p, P_PRIMITIVE,
                                                   sizeof(*primitive));
    if (primitive == NULL) return false;
    primitive->name = symbol;
    primitive->entry = entry;
    primitive->reserve = reserve;
    primitive->min_arity = min_arity;
    primitive->max_arity = max_arity;
    primitive->flags = flags;
    p_symbol_ptr(symbol)->function = (Value)primitive;
    p_symbol_ptr(symbol)->flags &= ~P_MACRO;
    return true;
}

bool p_add_primitive(PState *state, const char *name, bool standard,
                     PPrimitiveFn entry, PPrimitiveReserveFn reserve,
                     size_t min_arity, size_t max_arity, unsigned flags)
{
    if (state == NULL || !state->initialized || state->p == NULL ||
        name == NULL || entry == NULL) return false;
    size_t length = strlen(name);
    if (length > (SIZE_MAX - 2048u - sizeof(PPrimitive)) / 8u) {
        (void)p_error(state->p, P_ERR_RANGE, "primitive name is too long");
        return false;
    }
    size_t reserve_bytes = 2048u + length * 8u + sizeof(PPrimitive);
    if (!p_reserve(state->p, reserve_bytes)) return false;
    return add_primitive_reserved(state, name, standard, entry, reserve,
                                  min_arity, max_arity, flags);
}

bool p_install_primitives(PState *state)
{
    Pocket *p;
    size_t i;
    if (state == NULL || !state->initialized || state->p == NULL)
        return false;
    p = state->p;
    /* This is a single explicit safe point for the static descriptor table,
     * its symbol names, package entries, and initial special variables. */
    if (!p_reserve(p, 256u * 1024u))
        return false;
    for (i = 0; i < sizeof(primitive_specs) / sizeof(primitive_specs[0]); ++i) {
        const PrimitiveSpec *spec = &primitive_specs[i];
        if (!add_primitive_reserved(state, spec->name, spec->standard,
                                    spec->entry, spec->reserve,
                                    spec->min_arity, spec->max_arity,
                                    spec->flags)) return false;
    }
    if (!install_special_variable(p, "*PACKAGE*", p->user) ||
        !install_special_variable(p, "*FEATURES*", p->features) ||
        !install_special_variable(p, "*READ-EVAL*", p->nil) ||
        !install_special_variable(p, "*STANDARD-OUTPUT*", p->truth))
        return false;
    state->mv_count = 1u;
    state->mv[0] = p->nil;
    return true;
}
