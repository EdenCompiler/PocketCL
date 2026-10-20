#include "pocket.h"
#include "heap_internal.h"

#include <limits.h>
#include <string.h>

unsigned p_fixnum_bits(void) {
    unsigned width = (unsigned)(sizeof(Value) * CHAR_BIT);
    if (width == 32u) return 31u;
    if (width == 64u) return 62u;
    return width - 2u;
}

int64_t p_fixnum_min(void) {
    unsigned shift = p_fixnum_bits() - 1u;
    return -(INT64_C(1) << shift);
}

int64_t p_fixnum_max(void) {
    unsigned shift = p_fixnum_bits() - 1u;
    return (INT64_C(1) << shift) - 1;
}

Value p_fixnum(int64_t number) {
    unsigned bits = p_fixnum_bits();
    uintptr_t mask = (((uintptr_t)1u << bits) - 1u);
    return (((uintptr_t)number & mask) << 1u) | (uintptr_t)1u;
}

int64_t p_fixnum_value(Value value) {
    unsigned bits = p_fixnum_bits();
    uintptr_t mask = (((uintptr_t)1u << bits) - 1u);
    uintptr_t payload = (value >> 1u) & mask;
    uintptr_t sign = (uintptr_t)1u << (bits - 1u);
    if ((payload & sign) != 0u) {
        uintptr_t magnitude_minus_one = (~payload) & mask;
        return -1 - (int64_t)magnitude_minus_one;
    }
    return (int64_t)payload;
}

Value p_checked_fixnum(Pocket *p, int64_t number) {
    if (number < p_fixnum_min() || number > p_fixnum_max()) {
        (void)p_error(p, P_ERR_RANGE, "integer is outside the fixnum range");
        return 0;
    }
    return p_fixnum(number);
}

static bool size_for_payload(size_t header, size_t count, size_t element,
                             size_t *result) {
    size_t bytes;
    if (element != 0u && count > (SIZE_MAX - header) / element) return false;
    bytes = header + count * element;
    if (bytes > SIZE_MAX - 7u) return false;
    bytes = (bytes + 7u) & ~(size_t)7u;
    if (bytes < P_MIN_BLOCK) bytes = P_MIN_BLOCK;
    *result = bytes;
    return true;
}

Value p_cons(Pocket *p, Value car, Value cdr) {
    PCons *cell = (PCons *)p_alloc(p, P_CONS, sizeof(*cell));
    if (cell == NULL) return 0;
    cell->car = car;
    cell->cdr = cdr;
    return (Value)cell;
}

Value p_vector(Pocket *p, size_t length, Value initial) {
    size_t bytes;
    if (!size_for_payload(offsetof(PVector, data), length, sizeof(Value), &bytes)) {
        (void)p_error(p, P_ERR_RANGE, "vector is too large");
        return 0;
    }
    PVector *vector = (PVector *)p_alloc(p, P_VECTOR, bytes);
    if (vector == NULL) return 0;
    vector->length = length;
    for (size_t i = 0; i < length; ++i) vector->data[i] = initial;
    return (Value)vector;
}

Value p_bytes(Pocket *p, size_t length) {
    size_t bytes;
    if (!size_for_payload(offsetof(PBytes, data), length, sizeof(uint8_t), &bytes)) {
        (void)p_error(p, P_ERR_RANGE, "byte vector is too large");
        return 0;
    }
    PBytes *object = (PBytes *)p_alloc(p, P_BYTES, bytes);
    if (object == NULL) return 0;
    object->length = length;
    memset(object->data, 0, length);
    return (Value)object;
}

static bool valid_scalar(uint32_t codepoint) {
    return codepoint <= UINT32_C(0x10ffff) &&
           !(codepoint >= UINT32_C(0xd800) && codepoint <= UINT32_C(0xdfff));
}

Value p_string(Pocket *p, const uint32_t *text, size_t length) {
    size_t bytes;
    if ((length != 0u && text == NULL) ||
        !size_for_payload(offsetof(PString, data), length, sizeof(uint32_t), &bytes)) {
        (void)p_error(p, P_ERR_RANGE, "string is too large");
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        if (!valid_scalar(text[i])) {
            (void)p_error(p, P_ERR_RANGE, "string contains an invalid Unicode scalar");
            return 0;
        }
    }
    PString *string = (PString *)p_alloc(p, P_STRING, bytes);
    if (string == NULL) return 0;
    string->length = length;
    if (length != 0u) memcpy(string->data, text, length * sizeof(uint32_t));
    return (Value)string;
}

static bool decode_utf8(const unsigned char *input, size_t length,
                        uint32_t *output, size_t *count) {
    size_t i = 0u, n = 0u;
    while (i < length) {
        uint32_t cp;
        unsigned char a = input[i++];
        if (a < 0x80u) {
            cp = a;
        } else if (a >= 0xc2u && a <= 0xdfu) {
            if (i >= length || (input[i] & 0xc0u) != 0x80u) return false;
            cp = ((uint32_t)(a & 0x1fu) << 6u) | (uint32_t)(input[i++] & 0x3fu);
        } else if (a >= 0xe0u && a <= 0xefu) {
            unsigned char b, c;
            if (length - i < 2u) return false;
            b = input[i++]; c = input[i++];
            if ((b & 0xc0u) != 0x80u || (c & 0xc0u) != 0x80u ||
                (a == 0xe0u && b < 0xa0u) || (a == 0xedu && b >= 0xa0u)) return false;
            cp = ((uint32_t)(a & 0x0fu) << 12u) |
                 ((uint32_t)(b & 0x3fu) << 6u) | (uint32_t)(c & 0x3fu);
        } else if (a >= 0xf0u && a <= 0xf4u) {
            unsigned char b, c, d;
            if (length - i < 3u) return false;
            b = input[i++]; c = input[i++]; d = input[i++];
            if ((b & 0xc0u) != 0x80u || (c & 0xc0u) != 0x80u ||
                (d & 0xc0u) != 0x80u || (a == 0xf0u && b < 0x90u) ||
                (a == 0xf4u && b >= 0x90u)) return false;
            cp = ((uint32_t)(a & 7u) << 18u) | ((uint32_t)(b & 0x3fu) << 12u) |
                 ((uint32_t)(c & 0x3fu) << 6u) | (uint32_t)(d & 0x3fu);
        } else {
            return false;
        }
        if (output != NULL) output[n] = cp;
        ++n;
    }
    *count = n;
    return true;
}

Value p_string_utf8(Pocket *p, const char *text, size_t length) {
    if (length != 0u && text == NULL) {
        (void)p_error(p, P_ERR_RANGE, "UTF-8 input is null");
        return 0;
    }
    size_t count = 0u;
    if (!decode_utf8((const unsigned char *)text, length, NULL, &count)) {
        (void)p_error(p, P_ERR_READER, "invalid UTF-8 string");
        return 0;
    }
    size_t bytes;
    if (!size_for_payload(offsetof(PString, data), count, sizeof(uint32_t), &bytes)) {
        (void)p_error(p, P_ERR_RANGE, "UTF-8 string is too large");
        return 0;
    }
    PString *string = (PString *)p_alloc(p, P_STRING, bytes);
    if (string == NULL) return 0;
    string->length = count;
    size_t written = 0u;
    if (!decode_utf8((const unsigned char *)text, length, string->data, &written) ||
        written != count) {
        (void)p_error(p, P_ERR_READER, "invalid UTF-8 string");
        return 0;
    }
    return (Value)string;
}

bool p_string_equals_ascii(Value value, const char *ascii) {
    if (p_type(value) != P_STRING || ascii == NULL) return false;
    const PString *string = (const PString *)value;
    size_t i = 0u;
    for (; ascii[i] != '\0'; ++i) {
        if (i >= string->length || (unsigned char)ascii[i] > 0x7fu ||
            string->data[i] != (uint32_t)(unsigned char)ascii[i]) return false;
    }
    return i == string->length;
}

bool p_string_to_utf8(Pocket *p, Value value, char *out, size_t capacity,
                      size_t *written) {
    if (p_type(value) != P_STRING || (capacity != 0u && out == NULL)) {
        (void)p_error(p, P_ERR_TYPE, "expected a string and output buffer");
        return false;
    }
    const PString *string = (const PString *)value;
    size_t needed = 0u;
    for (size_t i = 0; i < string->length; ++i) {
        uint32_t cp = string->data[i];
        if (!valid_scalar(cp)) {
            (void)p_error(p, P_ERR_RANGE, "string contains an invalid Unicode scalar");
            return false;
        }
        size_t add = cp < 0x80u ? 1u : cp < 0x800u ? 2u : cp < 0x10000u ? 3u : 4u;
        if (needed > SIZE_MAX - add) {
            (void)p_error(p, P_ERR_RANGE, "UTF-8 output is too large");
            return false;
        }
        needed += add;
    }
    if (written != NULL) *written = needed;
    if (needed > capacity) {
        (void)p_error(p, P_ERR_RANGE, "UTF-8 output buffer is too small");
        return false;
    }
    size_t pos = 0u;
    for (size_t i = 0; i < string->length; ++i) {
        uint32_t cp = string->data[i];
        if (cp < 0x80u) out[pos++] = (char)cp;
        else if (cp < 0x800u) {
            out[pos++] = (char)(0xc0u | (cp >> 6u));
            out[pos++] = (char)(0x80u | (cp & 0x3fu));
        } else if (cp < 0x10000u) {
            out[pos++] = (char)(0xe0u | (cp >> 12u));
            out[pos++] = (char)(0x80u | ((cp >> 6u) & 0x3fu));
            out[pos++] = (char)(0x80u | (cp & 0x3fu));
        } else {
            out[pos++] = (char)(0xf0u | (cp >> 18u));
            out[pos++] = (char)(0x80u | ((cp >> 12u) & 0x3fu));
            out[pos++] = (char)(0x80u | ((cp >> 6u) & 0x3fu));
            out[pos++] = (char)(0x80u | (cp & 0x3fu));
        }
    }
    return true;
}

Value p_flonum(Pocket *p, double number) {
    PFlonum *object = (PFlonum *)p_alloc(p, P_FLONUM, sizeof(*object));
    if (object == NULL) return 0;
    object->number = number;
    return (Value)object;
}

Value p_record(Pocket *p, Value type, size_t length) {
    size_t bytes;
    if (!size_for_payload(offsetof(PRecord, data), length, sizeof(Value), &bytes)) {
        (void)p_error(p, P_ERR_RANGE, "record is too large");
        return 0;
    }
    PRecord *record = (PRecord *)p_alloc(p, P_RECORD, bytes);
    if (record == NULL) return 0;
    record->type = type;
    record->length = length;
    for (size_t i = 0; i < length; ++i) record->data[i] = 0;
    return (Value)record;
}

typedef struct { Value left, right; } EqualPair;

/* This bounded comparator supports reader/debug tests only. It does not
 * implement Lisp EQUAL; very large structures return false rather than
 * allocating host memory or risking unbounded C recursion. */
#define P_EQUAL_BUDGET 1024u

static bool push_pair(EqualPair *pairs, size_t *length,
                      Value left, Value right) {
    if (*length >= P_EQUAL_BUDGET) return false;
    pairs[(*length)++] = (EqualPair){ left, right };
    return true;
}

static bool seen_pair(const EqualPair *pairs, size_t length, Value a, Value b) {
    for (size_t i = 0; i < length; ++i)
        if (pairs[i].left == a && pairs[i].right == b) return true;
    return false;
}

bool p_equal(Value a, Value b) {
    EqualPair pending[P_EQUAL_BUDGET], seen[P_EQUAL_BUDGET];
    size_t pending_count = 0u, seen_count = 0u;
    bool equal = true;
    if (!push_pair(pending, &pending_count, a, b)) return false;
    while (pending_count != 0u && equal) {
        EqualPair pair = pending[--pending_count];
        a = pair.left; b = pair.right;
        if (a == b) continue;
        if (p_is_fixnum(a) || p_is_fixnum(b)) { equal = false; break; }
        if (p_is_character(a) || p_is_character(b)) { equal = false; break; }
        if (!p_is_pointer(a) || !p_is_pointer(b)) { equal = false; break; }
        PType ta = p_type(a), tb = p_type(b);
        if (ta != tb) { equal = false; break; }
        if (ta == P_STRING) {
            const PString *x = (const PString *)a, *y = (const PString *)b;
            equal = x->length == y->length &&
                    memcmp(x->data, y->data, x->length * sizeof(uint32_t)) == 0;
        } else if (ta == P_BYTES) {
            const PBytes *x = (const PBytes *)a, *y = (const PBytes *)b;
            equal = x->length == y->length && memcmp(x->data, y->data, x->length) == 0;
        } else if (ta == P_FLONUM) {
            equal = ((const PFlonum *)a)->number == ((const PFlonum *)b)->number;
        } else if (ta == P_CONS || ta == P_VECTOR || ta == P_RECORD) {
            if (seen_pair(seen, seen_count, a, b)) continue;
            if (!push_pair(seen, &seen_count, a, b)) { equal = false; break; }
            if (ta == P_CONS) {
                const PCons *x = (const PCons *)a, *y = (const PCons *)b;
                equal = push_pair(pending, &pending_count, x->car, y->car) &&
                        push_pair(pending, &pending_count, x->cdr, y->cdr);
            } else if (ta == P_VECTOR) {
                const PVector *x = (const PVector *)a, *y = (const PVector *)b;
                equal = x->length == y->length;
                for (size_t i = 0; equal && i < x->length; ++i)
                    equal = push_pair(pending, &pending_count, x->data[i], y->data[i]);
            } else {
                const PRecord *x = (const PRecord *)a, *y = (const PRecord *)b;
                equal = x->length == y->length &&
                        push_pair(pending, &pending_count, x->type, y->type);
                for (size_t i = 0; equal && i < x->length; ++i)
                    equal = push_pair(pending, &pending_count, x->data[i], y->data[i]);
            }
        } else {
            equal = false;
        }
    }
    return equal;
}
