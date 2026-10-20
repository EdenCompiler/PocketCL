/* Keep libc's locale-sensitive conversion functions isolated. The host starts
 * in the C locale; PocketCL never changes it. Debug output needs round trips,
 * while the full Lisp printer will supply shortest formatting at M6. */
#include "float_io.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool p_parse_flonum(const char *text, double *out)
{
    char *end;
    double number = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(number)) return false;
    *out = number;
    return true;
}

bool p_format_flonum(double value, char *out, size_t capacity)
{
    int length;
    size_t i;
    bool has_point = false;
    if (!isfinite(value)) return false;
    length = snprintf(out, capacity, "%.17g", value);
    if (length < 0 || (size_t)length >= capacity) return false;
    for (i = 0; i < (size_t)length; ++i) {
        if (out[i] == '.' || out[i] == 'e' || out[i] == 'E') has_point = true;
    }
    if (!has_point) {
        if ((size_t)length + 3 > capacity) return false;
        memcpy(out + length, ".0", 3);
    }
    return true;
}
