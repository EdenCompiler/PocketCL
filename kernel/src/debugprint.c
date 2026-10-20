/* A bounded diagnostic printer, intentionally independent of the later Lisp
 * printer. Cycles and deep trees have stable markers rather than addresses.
 * This code never allocates Lisp objects and therefore needs no safe point. */
#include "pocket.h"
#include "float_io.h"
#include <string.h>

typedef struct {
    Pocket *p;
    char *out;
    size_t used, capacity, depth;
    Value ancestors[256];
    bool failed;
} Printer;

static void character(Printer *w, char c)
{
    if (w->failed) return;
    if (w->used + 1 >= w->capacity) { w->failed = true; return; }
    w->out[w->used++] = c;
    w->out[w->used] = '\0';
}

static void literal(Printer *w, const char *s)
{
    while (*s) character(w, *s++);
}

static void unicode(Printer *w, uint32_t c)
{
    if (c < 0x80) character(w, (char)c);
    else if (c < 0x800) {
        character(w, (char)(0xc0 | (c >> 6)));
        character(w, (char)(0x80 | (c & 63)));
    } else if (c < 0x10000) {
        character(w, (char)(0xe0 | (c >> 12)));
        character(w, (char)(0x80 | ((c >> 6) & 63)));
        character(w, (char)(0x80 | (c & 63)));
    } else {
        character(w, (char)(0xf0 | (c >> 18)));
        character(w, (char)(0x80 | ((c >> 12) & 63)));
        character(w, (char)(0x80 | ((c >> 6) & 63)));
        character(w, (char)(0x80 | (c & 63)));
    }
}

static void integer(Printer *w, int64_t n, unsigned radix)
{
    char digits[64];
    size_t count = 0;
    uint64_t magnitude;
    if (n < 0) {
        character(w, '-');
        magnitude = (uint64_t)(-(n + 1)) + 1;
    } else magnitude = (uint64_t)n;
    do {
        digits[count++] = "0123456789ABCDEF"[magnitude % radix];
        magnitude /= radix;
    } while (magnitude);
    while (count) character(w, digits[--count]);
}

static bool bare_name(Value name)
{
    PString *s = p_string_ptr(name);
    size_t i;
    if (!s->length || (s->length == 1 && s->data[0] == '.')) return false;
    /* Escaping all digit-leading names avoids guessing the reader's complete
     * potential-number grammar (e.g. a symbol named 1E3). */
    if ((s->data[0] >= '0' && s->data[0] <= '9') || s->data[0] == '+' ||
        s->data[0] == '-' || s->data[0] == '.') return false;
    for (i = 0; i < s->length; ++i) {
        uint32_t c = s->data[i];
        if (c > 127 || (c >= 'a' && c <= 'z')) return false;
        if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '*' && c != '/' && c != '<' && c != '>' && c != '=' &&
            c != '_' && c != '-' && c != '+' && c != '$' && c != '%' &&
            c != '&' && c != '~' && c != '^' && c != '?' && c != '!') return false;
    }
    return true;
}

static void symbol_name(Printer *w, Value name)
{
    PString *s = p_string_ptr(name);
    size_t i;
    bool bare = bare_name(name);
    if (!bare) character(w, '|');
    for (i = 0; i < s->length; ++i) {
        if (!bare && (s->data[i] == '|' || s->data[i] == '\\')) character(w, '\\');
        unicode(w, s->data[i]);
    }
    if (!bare) character(w, '|');
}

static void print_value(Printer *w, Value value);

static void print_cons(Printer *w, Value list)
{
    size_t base = w->depth;
    character(w, '(');
    for (;;) {
        size_t i;
        print_value(w, p_cons_ptr(list)->car);
        list = p_cons_ptr(list)->cdr;
        if (list == w->p->nil) break;
        if (p_type(list) != P_CONS) {
            literal(w, " . ");
            print_value(w, list);
            break;
        }
        for (i = 0; i < w->depth; ++i) if (w->ancestors[i] == list) break;
        if (i != w->depth) { literal(w, " . #<CYCLE>"); break; }
        if (w->depth == 256) { literal(w, " . #<DEPTH-LIMIT>"); break; }
        w->ancestors[w->depth++] = list;
        character(w, ' ');
    }
    w->depth = base;
    character(w, ')');
}

static void print_value(Printer *w, Value value)
{
    size_t i;
    PType type;
    char number[64];
    if (w->failed) return;
    if (value == w->p->nil) { literal(w, "NIL"); return; }
    if (value == w->p->truth) { literal(w, "T"); return; }
    if (p_is_fixnum(value)) { integer(w, p_fixnum_value(value), 10); return; }
    if (p_is_character(value)) {
        uint32_t code = p_char_code(value);
        literal(w, "#\\");
        if (code == ' ') literal(w, "Space");
        else if (code == '\n') literal(w, "Newline");
        else if (code == '\t') literal(w, "Tab");
        else if (code == '\r') literal(w, "Return");
        else if (code >= 33 && code < 127) character(w, (char)code);
        else { character(w, 'x'); integer(w, code, 16); }
        return;
    }
    if (value == P_UNBOUND) { literal(w, "#<UNBOUND>"); return; }
    if (value == P_UNSUPPLIED) { literal(w, "#<UNSUPPLIED>"); return; }
    if (value == P_EOF) { literal(w, "#<EOF>"); return; }
    if (!p_is_pointer(value)) { literal(w, "#<INVALID-VALUE>"); return; }
    type = p_type(value);
    if (type == P_CONS || type == P_VECTOR || type == P_RECORD) {
        for (i = 0; i < w->depth; ++i) if (w->ancestors[i] == value) break;
        if (i != w->depth) { literal(w, "#<CYCLE>"); return; }
        if (w->depth == 256) { literal(w, "#<DEPTH-LIMIT>"); return; }
        w->ancestors[w->depth++] = value;
    }
    switch (type) {
    case P_CONS: print_cons(w, value); break;
    case P_VECTOR:
        literal(w, "#(");
        for (i = 0; i < p_vector_ptr(value)->length; ++i) {
            if (i) character(w, ' ');
            print_value(w, p_vector_ptr(value)->data[i]);
        }
        character(w, ')');
        break;
    case P_STRING:
        character(w, '"');
        for (i = 0; i < p_string_ptr(value)->length; ++i) {
            uint32_t code = p_string_ptr(value)->data[i];
            if (code == '"' || code == '\\') character(w, '\\');
            unicode(w, code);
        }
        character(w, '"');
        break;
    case P_SYMBOL: {
        PSymbol *symbol = p_symbol_ptr(value);
        if (symbol->home == w->p->nil) literal(w, "#:");
        else if (symbol->home == w->p->keyword) character(w, ':');
        else if (symbol->home != w->p->current_package) {
            symbol_name(w, ((PPackage *)symbol->home)->name);
            literal(w, "::");
        }
        symbol_name(w, symbol->name);
        break;
    }
    case P_FLONUM:
        if (p_format_flonum(((PFlonum *)value)->number, number, sizeof(number))) literal(w, number);
        else literal(w, "#<NONFINITE-FLOAT>");
        break;
    case P_PACKAGE:
        literal(w, "#<PACKAGE ");
        symbol_name(w, ((PPackage *)value)->name);
        character(w, '>');
        break;
    case P_RECORD:
        literal(w, "#S(");
        print_value(w, ((PRecord *)value)->type);
        for (i = 0; i < ((PRecord *)value)->length; ++i) {
            character(w, ' ');
            print_value(w, ((PRecord *)value)->data[i]);
        }
        character(w, ')');
        break;
    case P_BYTES: literal(w, "#<BYTE-VECTOR "); integer(w, (int64_t)((PBytes *)value)->length, 10); character(w, '>'); break;
    case P_CLOSURE: literal(w, "#<FUNCTION>"); break;
    case P_PROTO: literal(w, "#<PROTOTYPE>"); break;
    case P_PRIMITIVE: literal(w, "#<PRIMITIVE>"); break;
    case P_HASHTABLE: literal(w, "#<HASH-TABLE>"); break;
    case P_BOX: literal(w, "#<BOX>"); break;
    case P_RECORD_TYPE: literal(w, "#<RECORD-TYPE>"); break;
    case P_ARRAY_HEADER: literal(w, "#<ARRAY>"); break;
    default: literal(w, "#<INTERNAL-OBJECT>"); break;
    }
    if (type == P_CONS || type == P_VECTOR || type == P_RECORD) --w->depth;
}

bool p_debug_print(Pocket *p, Value value, char *out, size_t capacity)
{
    Printer printer;
    memset(&printer, 0, sizeof(printer));
    printer.p = p;
    printer.out = out;
    printer.capacity = capacity;
    if (!capacity) { p_error(p, P_ERR_RANGE, "debug printer output buffer is empty"); return false; }
    out[0] = '\0';
    print_value(&printer, value);
    if (printer.failed) { p_error(p, P_ERR_RANGE, "debug printer output buffer exhausted"); return false; }
    return true;
}
