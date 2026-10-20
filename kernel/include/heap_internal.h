#ifndef POCKET_HEAP_INTERNAL_H
#define POCKET_HEAP_INTERNAL_H

#include "pocket.h"

/* Header fields are deliberately decoded in one place. The low byte is the
 * public object type, bit 8 is transient GC state, and upper bits hold bytes. */
#define P_HEADER_TYPE_MASK ((uintptr_t)255u)
#define P_HEADER_MARK ((uintptr_t)256u)
#define P_HEADER_SIZE_SHIFT 10u
#define P_SIZE_UNIT 8u
#define P_MIN_BLOCK ((size_t)(2u * sizeof(uintptr_t) > 16u ? \
                            2u * sizeof(uintptr_t) : 16u))
#define P_SMALL_LIMIT ((size_t)256u)

static inline uintptr_t p_header_make(PType type, size_t size) {
    return (((uintptr_t)(size / P_SIZE_UNIT)) << P_HEADER_SIZE_SHIFT) |
           (uintptr_t)type;
}

static inline PType p_header_type(const PObject *object) {
    return (PType)(object->header & P_HEADER_TYPE_MASK);
}

void p_heap_rebuild_bins(Pocket *p);
bool p_heap_contains(const Pocket *p, const void *address);

#endif
