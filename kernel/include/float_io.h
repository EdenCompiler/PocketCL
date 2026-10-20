#ifndef POCKET_FLOAT_IO_H
#define POCKET_FLOAT_IO_H
#include <stdbool.h>
#include <stddef.h>
bool p_parse_flonum(const char *text, double *out);
bool p_format_flonum(double value, char *out, size_t capacity);
#endif
