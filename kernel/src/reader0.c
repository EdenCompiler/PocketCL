/* Fixed-readtable bootstrap reader.
 *
 * Parsing is bounded to avoid exhausting the host C stack on hostile input.
 * Each recursive form registers its small local Value array before a safe
 * point; constructors themselves never run GC. Suppression parses syntax but
 * does not intern names or execute #., including inside discarded features.
 */
#include "pocket.h"
#include "float_io.h"
#include <string.h>

#define READER_DEPTH 256
#define TOKEN_CAPACITY 4096
#define READ_SKIP ((Value)34)

typedef struct {
    char text[TOKEN_CAPACITY];
    bool escaped[TOKEN_CAPACITY];
    size_t length;
    bool any_escape;
} Token;

static Value read_form(PReader *r, bool suppress);
static Value read_required(PReader *r, bool suppress);

static Value read_error(PReader *r, PErrorCode code, const char *message)
{
    p_error(r->p, code, message);
    r->p->error.line = r->line;
    r->p->error.column = r->column;
    return 0;
}

static int peek(const PReader *r)
{
    return r->offset < r->length ? (unsigned char)r->source[r->offset] : -1;
}

static int take(PReader *r)
{
    int c = peek(r);
    if (c >= 0) {
        ++r->offset;
        if (c == '\n') { ++r->line; r->column = 1; }
        else ++r->column;
    }
    return c;
}

static bool space(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static bool delimiter(int c)
{
    return c < 0 || space(c) || c == '(' || c == ')' || c == '"' ||
           c == ';' || c == '\'' || c == '`' || c == ',';
}

static bool skip_space(PReader *r)
{
    for (;;) {
        int c = peek(r);
        if (space(c)) { take(r); continue; }
        if (c == ';') {
            while (peek(r) >= 0 && take(r) != '\n') { }
            continue;
        }
        if (c == '#' && r->offset + 1 < r->length &&
            r->source[r->offset + 1] == '|') {
            size_t nesting = 1;
            take(r); take(r);
            while (nesting) {
                c = take(r);
                if (c < 0) { read_error(r, P_ERR_READER, "unterminated block comment"); return false; }
                if (c == '#' && peek(r) == '|') { take(r); ++nesting; }
                else if (c == '|' && peek(r) == '#') { take(r); --nesting; }
            }
            continue;
        }
        return true;
    }
}

static bool token_append(PReader *r, Token *t, int c, bool escaped)
{
    if (c <= 0) { read_error(r, P_ERR_READER, "NUL or end of input inside token"); return false; }
    if (t->length + 1 >= TOKEN_CAPACITY) {
        read_error(r, P_ERR_READER, "bootstrap token exceeds 4095 bytes"); return false;
    }
    t->text[t->length] = (char)c;
    t->escaped[t->length++] = escaped;
    t->any_escape = t->any_escape || escaped;
    t->text[t->length] = '\0';
    return true;
}

static bool read_token(PReader *r, Token *t)
{
    bool bars = false;
    memset(t, 0, sizeof(*t));
    for (;;) {
        int c = peek(r);
        if (c < 0 || (!bars && delimiter(c))) break;
        take(r);
        if (c == '\\') {
            if (!token_append(r, t, take(r), true)) return false;
        } else if (c == '|') {
            bars = !bars;
            t->any_escape = true;
        } else {
            if (!bars && c >= 'a' && c <= 'z') c -= 'a' - 'A';
            if (!token_append(r, t, c, bars)) return false;
        }
    }
    if (bars) { read_error(r, P_ERR_READER, "unterminated multiple escape"); return false; }
    /* The v1 character tables cover Latin-1 as well as ASCII. */
    {
        size_t i;
        for (i = 0; i + 1 < t->length; ++i) {
            unsigned char a = (unsigned char)t->text[i];
            unsigned char b = (unsigned char)t->text[i + 1];
            if (!t->escaped[i] && !t->escaped[i + 1] && a == 0xc3 &&
                ((b >= 0xa0 && b <= 0xb6) || (b >= 0xb8 && b <= 0xbe)))
                t->text[i + 1] = (char)(b - 32);
        }
    }
    return true;
}

static int digit_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* Return false only when the token is not an integer. Range errors set p.error
 * and count as recognized numbers, so they cannot silently become symbols. */
static bool integer_token(PReader *r, const char *s, unsigned base, Value *out)
{
    const char *q = s;
    uint64_t magnitude = 0, limit;
    bool negative = false;
    int digit;
    if (*q == '+' || *q == '-') { negative = *q == '-'; ++q; }
    if (!*q) return false;
    {
        const char *check = q;
        while (*check) {
            digit = digit_value((unsigned char)*check++);
            if (digit < 0 || (unsigned)digit >= base) return false;
        }
    }
    limit = negative ? (uint64_t)(-(p_fixnum_min() + 1)) + 1 : (uint64_t)p_fixnum_max();
    while (*q) {
        digit = digit_value((unsigned char)*q++);
        if (magnitude > (limit - (unsigned)digit) / base) {
            *out = read_error(r, P_ERR_RANGE, "integer literal exceeds target fixnum range");
            return true;
        }
        magnitude = magnitude * base + (unsigned)digit;
    }
    *out = p_fixnum(negative ? -(int64_t)magnitude : (int64_t)magnitude);
    return true;
}

static Value token_symbol(PReader *r, Token *t, bool suppress, bool uninterned)
{
    Pocket *p = r->p;
    size_t i, colon = t->length;
    Value package;
    int status;
    if (suppress) return p->nil;
    if (!p_reserve(p, t->length * 8 + 1024)) return 0;
    if (uninterned) return p_make_symbol(p, t->text);
    for (i = 0; i < t->length; ++i) {
        if (t->text[i] == ':' && !t->escaped[i]) { colon = i; break; }
    }
    if (colon == t->length) return p_intern(p, p->current_package, t->text, NULL);
    if (colon == 0) {
        for (i = 1; i < t->length; ++i)
            if (t->text[i] == ':' && !t->escaped[i])
                return read_error(r, P_ERR_READER, "invalid keyword package marker");
        if (t->length == 1) return read_error(r, P_ERR_READER, "missing keyword name");
        return p_intern(p, p->keyword, t->text + 1, NULL);
    }
    t->text[colon] = '\0';
    package = p_find_package(p, t->text);
    if (!package) return read_error(r, P_ERR_PACKAGE, "unknown package in qualified symbol");
    i = colon + 1;
    if (i < t->length && t->text[i] == ':' && !t->escaped[i]) {
        ++i;
        if (i == t->length) return read_error(r, P_ERR_READER, "missing symbol after package marker");
        {
            size_t j;
            for (j = i; j < t->length; ++j)
                if (t->text[j] == ':' && !t->escaped[j])
                    return read_error(r, P_ERR_READER, "too many package markers");
        }
        return p_intern(p, package, t->text + i, NULL);
    }
    if (i == t->length) return read_error(r, P_ERR_READER, "missing symbol after package marker");
    for (colon = i; colon < t->length; ++colon)
        if (t->text[colon] == ':' && !t->escaped[colon])
            return read_error(r, P_ERR_READER, "too many package markers");
    {
        Value result = p_find_symbol(p, package, t->text + i, &status);
        if (!result) return 0;
        if (status != 2) return read_error(r, P_ERR_PACKAGE, "symbol is not exported from package");
        return result;
    }
}

static Value read_atom(PReader *r, bool suppress)
{
    Token token;
    Value result;
    char numeric[TOKEN_CAPACITY];
    size_t i;
    bool float_syntax = false, ratio_syntax = false, candidate;
    double number;
    if (!read_token(r, &token)) return 0;
    if (!token.length && !token.any_escape) return read_error(r, P_ERR_READER, "expected a token");
    if (suppress) return r->p->nil;
    if (!token.any_escape) {
        if (integer_token(r, token.text, 10, &result)) return result;
        /* A trailing decimal point denotes an integer in Common Lisp. */
        if (token.length > 1 && token.text[token.length - 1] == '.') {
            token.text[token.length - 1] = '\0';
            if (integer_token(r, token.text, 10, &result)) return result;
            token.text[token.length - 1] = '.';
        }
        candidate = (token.text[0] >= '0' && token.text[0] <= '9') ||
            ((token.text[0] == '+' || token.text[0] == '-' || token.text[0] == '.') &&
             ((token.text[1] >= '0' && token.text[1] <= '9') || token.text[1] == '.'));
        for (i = 0; i <= token.length; ++i) {
            char c = token.text[i];
            if (c == '/') ratio_syntax = true;
            if (c == '.' || c == 'E' || c == 'D' || c == 'F' || c == 'S' || c == 'L') {
                float_syntax = true;
                if (c != '.') c = 'e';
            }
            numeric[i] = c;
        }
        if (candidate && float_syntax && p_parse_flonum(numeric, &number)) {
            if (!p_reserve(r->p, sizeof(PFlonum))) return 0;
            return p_flonum(r->p, number);
        }
        if (candidate && ratio_syntax)
            return read_error(r, P_ERR_UNSUPPORTED, "ratios are unsupported; use floating-point division");
        if (strcmp(token.text, ".") == 0)
            return read_error(r, P_ERR_READER, "dot outside a dotted list");
    }
    return token_symbol(r, &token, false, false);
}

static Value read_string(PReader *r, bool suppress)
{
    char text[TOKEN_CAPACITY];
    size_t length = 0;
    int c;
    take(r);
    while ((c = take(r)) != '"') {
        if (c < 0) return read_error(r, P_ERR_READER, "unterminated string");
        if (c == '\\') {
            c = take(r);
            if (c < 0) return read_error(r, P_ERR_READER, "unterminated string escape");
        }
        if (c == 0) return read_error(r, P_ERR_READER, "NUL inside source string");
        if (length + 1 >= sizeof(text)) return read_error(r, P_ERR_READER, "bootstrap string exceeds 4095 bytes");
        text[length++] = (char)c;
    }
    if (suppress) return r->p->nil;
    if (!p_reserve(r->p, sizeof(PString) + length * sizeof(uint32_t))) return 0;
    return p_string_utf8(r->p, text, length);
}

static Value read_list(PReader *r, bool suppress, bool vector)
{
    Pocket *p = r->p;
    Value roots[4] = {p->nil, p->nil, p->nil, p->nil};
    size_t saved = p_root_save(p), count = 0;
    bool ok = false;
    if (!p_roots(p, roots, 4)) return 0;
    take(r); /* opening parenthesis */
    for (;;) {
        if (!skip_space(r)) break;
        if (peek(r) < 0) { read_error(r, P_ERR_READER, "unterminated list"); break; }
        if (peek(r) == ')') { take(r); ok = true; break; }
        if (peek(r) == '.' &&
            (r->offset + 1 == r->length || delimiter((unsigned char)r->source[r->offset + 1]))) {
            take(r);
            if (vector || (!suppress && roots[0] == p->nil) || count == 0) {
                read_error(r, P_ERR_READER, "misplaced dotted-list marker"); break;
            }
            roots[2] = read_required(r, suppress);
            if (!roots[2] || roots[2] == P_EOF) { if (roots[2]) read_error(r, P_ERR_READER, "missing dotted tail"); break; }
            if (!skip_space(r)) break;
            if (peek(r) != ')') { read_error(r, P_ERR_READER, "dotted list must have exactly one tail"); break; }
            take(r);
            if (!suppress) p_cons_ptr(roots[1])->cdr = roots[2];
            ok = true;
            break;
        }
        roots[2] = read_form(r, suppress);
        if (!roots[2]) break;
        if (roots[2] == READ_SKIP) continue;
        ++count;
        if (!suppress) {
            if (!p_reserve(p, sizeof(PCons))) break;
            roots[3] = p_cons(p, roots[2], p->nil);
            if (!roots[3]) break;
            if (roots[0] == p->nil) roots[0] = roots[3];
            else p_cons_ptr(roots[1])->cdr = roots[3];
            roots[1] = roots[3];
        }
    }
    if (ok && vector && !suppress) {
        size_t i;
        Value cursor;
        if (count > (SIZE_MAX - sizeof(PVector)) / sizeof(Value) ||
            !p_reserve(p, sizeof(PVector) + count * sizeof(Value))) ok = false;
        else {
            roots[3] = p_vector(p, count, p->nil);
            if (!roots[3]) ok = false;
            else for (cursor = roots[0], i = 0; i < count; ++i, cursor = p_cons_ptr(cursor)->cdr)
                p_vector_ptr(roots[3])->data[i] = p_cons_ptr(cursor)->car;
            roots[0] = roots[3];
        }
    }
    p_root_restore(p, saved);
    return ok ? (suppress ? p->nil : roots[0]) : 0;
}

static Value read_prefix(PReader *r, bool suppress, Value package, const char *name)
{
    Pocket *p = r->p;
    Value roots[3] = {p->nil, p->nil, p->nil};
    size_t saved = p_root_save(p);
    if (!p_roots(p, roots, 3)) return 0;
    roots[0] = read_required(r, suppress);
    if (roots[0] == P_EOF) roots[0] = read_error(r, P_ERR_READER, "missing form after reader prefix");
    if (roots[0] && !suppress && p_reserve(p, 2048)) {
        roots[1] = p_intern(p, package, name, NULL);
        if (roots[1]) roots[2] = p_cons(p, roots[0], p->nil);
        if (roots[2]) roots[0] = p_cons(p, roots[1], roots[2]);
        else roots[0] = 0;
    } else if (suppress && roots[0]) roots[0] = p->nil;
    else roots[0] = 0;
    p_root_restore(p, saved);
    return roots[0];
}

static bool feature_test(PReader *r, Value form, bool *answer, size_t depth)
{
    Pocket *p = r->p;
    Value cursor, name;
    bool conjunction, disjunction, result, value;
    if (depth > READER_DEPTH) { read_error(r, P_ERR_READER, "feature expression nesting limit"); return false; }
    if (p_type(form) == P_SYMBOL) {
        for (cursor = p->features; p_type(cursor) == P_CONS; cursor = p_cons_ptr(cursor)->cdr)
            if (p_cons_ptr(cursor)->car == form) { *answer = true; return true; }
        *answer = false;
        return true;
    }
    if (p_type(form) != P_CONS || p_type(p_cons_ptr(form)->car) != P_SYMBOL) {
        read_error(r, P_ERR_READER, "invalid feature expression"); return false;
    }
    name = p_symbol_ptr(p_cons_ptr(form)->car)->name;
    cursor = p_cons_ptr(form)->cdr;
    if (p_string_equals_ascii(name, "NOT")) {
        if (p_type(cursor) != P_CONS || p_cons_ptr(cursor)->cdr != p->nil) {
            read_error(r, P_ERR_READER, "NOT feature expression requires one argument"); return false;
        }
        if (!feature_test(r, p_cons_ptr(cursor)->car, &value, depth + 1)) return false;
        *answer = !value;
        return true;
    }
    conjunction = p_string_equals_ascii(name, "AND");
    disjunction = p_string_equals_ascii(name, "OR");
    if (!conjunction && !disjunction) { read_error(r, P_ERR_READER, "unknown feature expression operator"); return false; }
    result = conjunction;
    while (p_type(cursor) == P_CONS) {
        if (!feature_test(r, p_cons_ptr(cursor)->car, &value, depth + 1)) return false;
        if (conjunction) result = result && value;
        else result = result || value;
        cursor = p_cons_ptr(cursor)->cdr;
    }
    if (cursor != p->nil) { read_error(r, P_ERR_READER, "improper feature expression"); return false; }
    *answer = result;
    return true;
}

static Value read_dispatch(PReader *r, bool suppress)
{
    Pocket *p = r->p;
    int dispatch;
    Token token;
    Value result;
    take(r);
    dispatch = take(r);
    if (dispatch >= 'a' && dispatch <= 'z') dispatch -= 'a' - 'A';
    if (dispatch == '(') { --r->offset; --r->column; return read_list(r, suppress, true); }
    if (dispatch == '\'') return read_prefix(r, suppress, p->cl, "FUNCTION");
    if (dispatch == ':') {
        if (!read_token(r, &token)) return 0;
        if (!token.length && !token.any_escape) return read_error(r, P_ERR_READER, "missing uninterned symbol name");
        return token_symbol(r, &token, suppress, true);
    }
    if (dispatch == 'X' || dispatch == 'B' || dispatch == 'O') {
        unsigned base = dispatch == 'X' ? 16u : dispatch == 'B' ? 2u : 8u;
        if (!read_token(r, &token)) return 0;
        if (suppress) return p->nil;
        if (!token.any_escape && integer_token(r, token.text, base, &result)) return result;
        return read_error(r, P_ERR_READER, "invalid radix integer literal");
    }
    if (dispatch == '\\') {
        uint32_t code = 0;
        size_t i;
        if (peek(r) < 0) return read_error(r, P_ERR_READER, "missing character name");
        memset(&token, 0, sizeof(token));
        /* A single delimiter character is legal immediately after #\\. */
        if (!token_append(r, &token, take(r), true)) return 0;
        while (!delimiter(peek(r))) {
            if (!token_append(r, &token, take(r), true)) return 0;
        }
        if (suppress) return p->nil;
        for (i = 0; i < token.length; ++i)
            if (token.text[i] >= 'a' && token.text[i] <= 'z') token.text[i] -= 'a' - 'A';
        if (token.length == 1) code = (unsigned char)r->source[r->offset - 1];
        else if (strcmp(token.text, "SPACE") == 0) code = ' ';
        else if (strcmp(token.text, "NEWLINE") == 0) code = '\n';
        else if (strcmp(token.text, "TAB") == 0) code = '\t';
        else if (strcmp(token.text, "RETURN") == 0) code = '\r';
        else if (token.text[0] == 'X' || (token.text[0] == 'U' && token.text[1] == '+')) {
            i = token.text[0] == 'X' ? 1 : 2;
            if (i == token.length) return read_error(r, P_ERR_READER, "missing hexadecimal character code");
            for (; i < token.length; ++i) {
                int digit = digit_value(token.text[i]);
                if (digit < 0 || digit >= 16 || code > 0x10ffffu / 16)
                    return read_error(r, P_ERR_READER, "invalid hexadecimal character code");
                code = code * 16 + (unsigned)digit;
            }
        } else {
            /* Let the UTF-8 constructor perform validation for a literal codepoint. */
            Value string;
            if (!p_reserve(p, sizeof(PString) + token.length * 4)) return 0;
            string = p_string_utf8(p, token.text, token.length);
            if (!string) return 0;
            if (p_string_ptr(string)->length != 1) return read_error(r, P_ERR_READER, "unknown character name");
            code = p_string_ptr(string)->data[0];
        }
        if (code > 0x10ffffu || (code >= 0xd800u && code <= 0xdfffu))
            return read_error(r, P_ERR_READER, "character is not a Unicode scalar value");
        return p_character(code);
    }
    if (dispatch == '+' || dispatch == '-') {
        Value roots[2] = {p->nil, p->current_package};
        size_t saved = p_root_save(p);
        bool enabled = false;
        if (!p_roots(p, roots, 2)) return 0;
        p->current_package = p->keyword;
        roots[0] = read_required(r, suppress);
        p->current_package = roots[1];
        if (!roots[0] || roots[0] == P_EOF) {
            p_root_restore(p, saved);
            return read_error(r, P_ERR_READER, "missing feature expression");
        }
        if (!suppress && !feature_test(r, roots[0], &enabled, 0)) { p_root_restore(p, saved); return 0; }
        if (dispatch == '-') enabled = !enabled;
        roots[0] = read_required(r, suppress || !enabled);
        if (roots[0] == P_EOF) roots[0] = read_error(r, P_ERR_READER, "missing conditional form");
        result = roots[0] && !suppress && !enabled ? READ_SKIP : roots[0];
        p_root_restore(p, saved);
        return result;
    }
    if (dispatch == '.') {
        Value value = read_required(r, suppress);
        if (!value || value == P_EOF) return read_error(r, P_ERR_READER, "missing read-time evaluation form");
        if (suppress) return p->nil;
        if (!r->allow_read_eval || !r->read_eval)
            return read_error(r, P_ERR_READER, "read-time evaluation disabled (*READ-EVAL* is NIL)");
        return r->read_eval(p, value);
    }
    return read_error(r, P_ERR_READER, "unsupported sharpsign reader dispatch");
}

static Value read_form(PReader *r, bool suppress)
{
    Value result;
    if (++r->depth > READER_DEPTH) {
        --r->depth;
        return read_error(r, P_ERR_READER, "bootstrap reader nesting exceeds 256 forms");
    }
    if (!skip_space(r)) { --r->depth; return 0; }
    switch (peek(r)) {
    case -1: result = P_EOF; break;
    case 0: result = read_error(r, P_ERR_READER, "NUL in source input"); break;
    case ')': result = read_error(r, P_ERR_READER, "unexpected closing parenthesis"); break;
    case '(': result = read_list(r, suppress, false); break;
    case '"': result = read_string(r, suppress); break;
    case '#': result = read_dispatch(r, suppress); break;
    case '\'': take(r); result = read_prefix(r, suppress, r->p->cl, "QUOTE"); break;
    case '`':
        take(r);
        ++r->quasiquote_depth;
        result = read_prefix(r, suppress, r->p->sys, "%QUASIQUOTE");
        --r->quasiquote_depth;
        break;
    case ',':
        take(r);
        if (!r->quasiquote_depth) {
            result = read_error(r, P_ERR_READER, "comma outside backquote");
            break;
        }
        --r->quasiquote_depth;
        if (peek(r) == '@') { take(r); result = read_prefix(r, suppress, r->p->sys, "%UNQUOTE-SPLICING"); }
        else result = read_prefix(r, suppress, r->p->sys, "%UNQUOTE");
        ++r->quasiquote_depth;
        break;
    default: result = read_atom(r, suppress); break;
    }
    --r->depth;
    return result;
}

static Value read_required(PReader *r, bool suppress)
{
    Value value;
    do { value = read_form(r, suppress); } while (value == READ_SKIP);
    return value;
}

void p_reader_init(PReader *r, Pocket *p, const char *source, size_t length)
{
    memset(r, 0, sizeof(*r));
    r->p = p;
    r->source = source;
    r->length = length;
    r->line = 1;
    r->column = 1;
}

Value p_read(PReader *r)
{
    Value value = read_required(r, false);
    /* Constructor/package failures originate below the reader, where source
     * positions are unknown. Attach the current position at this boundary. */
    if (!value && !r->p->error.line) {
        r->p->error.line = r->line;
        r->p->error.column = r->column;
    }
    return value;
}
