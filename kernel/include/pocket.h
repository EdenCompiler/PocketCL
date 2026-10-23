#ifndef POCKET_H
#define POCKET_H

/* Public kernel vocabulary. Read this before the implementation files.
 * Values never move. Allocators never collect: the evaluator/VM (or a C caller)
 * first registers live Values, then calls p_reserve at an explicit safe point.
 * A Value of zero is an internal failure sentinel, never the Lisp NIL value. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uintptr_t Value;

typedef enum {
    P_FREE, P_CONS, P_VECTOR, P_STRING, P_BYTES, P_SYMBOL, P_PACKAGE,
    P_FLONUM, P_CLOSURE, P_PROTO, P_PRIMITIVE, P_HASHTABLE, P_RECORD,
    P_RECORD_TYPE, P_BOX, P_ARRAY_HEADER, P_PACKAGE_ENTRY
} PType;

typedef enum {
    P_OK, P_ERR_HEAP, P_ERR_TYPE, P_ERR_RANGE, P_ERR_READER, P_ERR_PACKAGE,
    P_ERR_IO, P_ERR_ARITY, P_ERR_UNBOUND, P_ERR_UNDEFINED, P_ERR_CONTROL,
    P_ERR_STACK, P_ERR_UNSUPPORTED, P_ERR_IMAGE
} PErrorCode;

typedef struct {
    PErrorCode code;
    size_t line, column;
    Value datum, expected;
    char message[256];
    /* Lisp ERROR already dispatched this condition. Returning to the host
     * must not invoke the kernel hook and dispatch the same handlers twice. */
    bool signaled;
} PError;

/* One-word object header: low byte type, mark bit 8, and byte size in
 * eight-byte units beginning at bit 10. Bit 9 stays reserved.
 * Every block is at least two words, and all starts/sizes are 8-byte aligned.
 * Free blocks use their payload's first word as the intrusive free-list link. */
typedef struct { uintptr_t header; } PObject;
typedef struct { uintptr_t header; Value car, cdr; } PCons;
typedef struct { uintptr_t header; size_t length; Value data[]; } PVector;
typedef struct { uintptr_t header; size_t length; uint32_t data[]; } PString;
typedef struct { uintptr_t header; size_t length; uint8_t data[]; } PBytes;
typedef struct { uintptr_t header; double number; } PFlonum;
typedef struct {
    uintptr_t header;
    Value name, home, value, function, plist;
    unsigned flags;
} PSymbol;
typedef struct {
    uintptr_t header;
    Value name, nicknames, entries, entry_tail, uses, shadows, next;
} PPackage;
typedef struct {
    uintptr_t header;
    Value symbol, next;
    bool external;
} PPackageEntry;
typedef struct {
    uintptr_t header;
    Value type;
    size_t length;
    Value data[];
} PRecord;

enum { P_SPECIAL = 1, P_CONSTANT = 2, P_MACRO = 4, P_SPECIAL_OPERATOR = 8 };
enum { P_UNBOUND = 2, P_UNSUPPLIED = 10, P_EOF = 18 };
#define P_CHAR_TAG ((Value)26)
#define P_MARK ((uintptr_t)256)
#define P_ROOT_LIMIT 2048
#define P_FREE_BINS 33

typedef struct { Value *values; size_t count; } PRoot;
typedef struct Pocket {
    void *allocation;
    uint8_t *heap;
    size_t heap_size, arena_size, live_bytes, allocated_since_gc, collections;
    PObject *free_bins[P_FREE_BINS];
    Value *mark_stack;
    size_t mark_capacity;
    PRoot roots[P_ROOT_LIMIT];
    size_t root_count;
    Value nil, truth, packages, package_tail, cl, user, keyword, sys;
    Value current_package, features, error_hook;
    PError error;
    bool gc_stress, initializing;
} Pocket;

static inline bool p_is_fixnum(Value v) { return (v & 1u) != 0; }
static inline bool p_is_character(Value v) { return (v & 255u) == P_CHAR_TAG; }
static inline Value p_character(uint32_t c) { return ((Value)c << 8) | P_CHAR_TAG; }
static inline uint32_t p_char_code(Value v) { return (uint32_t)(v >> 8); }
static inline bool p_is_pointer(Value v) { return v != 0 && (v & 7u) == 0; }
static inline PType p_type(Value v) {
    return p_is_pointer(v) ? (PType)(((PObject *)v)->header & 255u) : P_FREE;
}
static inline size_t p_object_size(const PObject *v) {
    return (size_t)(v->header >> 10) * 8u;
}
static inline PCons *p_cons_ptr(Value v) { return (PCons *)v; }
static inline PVector *p_vector_ptr(Value v) { return (PVector *)v; }
static inline PString *p_string_ptr(Value v) { return (PString *)v; }
static inline PSymbol *p_symbol_ptr(Value v) { return (PSymbol *)v; }

/* fixnum_bits includes the sign bit. Encoding uses unsigned arithmetic only. */
unsigned p_fixnum_bits(void);
int64_t p_fixnum_min(void);
int64_t p_fixnum_max(void);
Value p_fixnum(int64_t number);
int64_t p_fixnum_value(Value value);
Value p_checked_fixnum(Pocket *p, int64_t number);

bool p_init(Pocket *p, size_t arena_size);
void p_destroy(Pocket *p);
void p_clear_error(Pocket *p);
Value p_error(Pocket *p, PErrorCode code, const char *message);
void *p_alloc(Pocket *p, PType type, size_t bytes);
bool p_reserve(Pocket *p, size_t bytes);
void p_gc(Pocket *p);
size_t p_root_save(const Pocket *p);
bool p_roots(Pocket *p, Value *values, size_t count);
void p_root_restore(Pocket *p, size_t saved);

Value p_cons(Pocket *p, Value car, Value cdr);
Value p_vector(Pocket *p, size_t length, Value initial);
Value p_bytes(Pocket *p, size_t length);
Value p_string(Pocket *p, const uint32_t *text, size_t length);
Value p_string_utf8(Pocket *p, const char *text, size_t length);
bool p_string_equals_ascii(Value string, const char *ascii);
bool p_string_to_utf8(Pocket *p, Value string, char *out, size_t capacity, size_t *written);
Value p_flonum(Pocket *p, double number);
Value p_record(Pocket *p, Value type, size_t length);
bool p_equal(Value a, Value b);

/* Package entries own export status, not symbols: imports can be exported
 * independently in each package. Iteration follows entry insertion order.
 * find_symbol status: 0 absent, 1 internal, 2 external, 3 inherited. */
bool p_packages_init(Pocket *p);
Value p_make_package(Pocket *p, const char *name);
Value p_find_package(Pocket *p, const char *name);
Value p_find_symbol(Pocket *p, Value package, const char *name, int *status);
Value p_intern(Pocket *p, Value package, const char *name, int *status);
Value p_make_symbol(Pocket *p, const char *name);
bool p_export(Pocket *p, Value package, Value symbol);
bool p_unexport(Pocket *p, Value package, Value symbol);
bool p_import(Pocket *p, Value package, Value symbol, bool shadowing);
bool p_shadow(Pocket *p, Value package, const char *name);
bool p_use_package(Pocket *p, Value package, Value used);
bool p_unuse_package(Pocket *p, Value package, Value used);

/* The bootstrap reader accepts UTF-8 bytes, keeps precise source positions,
 * and uses fixed-capacity registered scratch roots. It never executes #.
 * unless an explicit evaluator callback is installed by stage 0. */
typedef Value (*PReadEval)(Pocket *, Value);
typedef struct {
    Pocket *p;
    const char *source;
    size_t length, offset, line, column, depth, quasiquote_depth;
    PReadEval read_eval;
    bool allow_read_eval;
} PReader;
void p_reader_init(PReader *r, Pocket *p, const char *source, size_t length);
Value p_read(PReader *r);
bool p_debug_print(Pocket *p, Value value, char *out, size_t capacity);

#endif
