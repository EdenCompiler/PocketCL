#include "pocket.h"
#include "heap_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static size_t align_block(size_t bytes) {
    if (bytes > SIZE_MAX - 7u) return 0u;
    bytes = (bytes + 7u) & ~(size_t)7u;
    return bytes < P_MIN_BLOCK ? P_MIN_BLOCK : bytes;
}

static size_t max_block_size(void) {
    size_t maximum = (size_t)(UINTPTR_MAX >> P_HEADER_SIZE_SHIFT) * P_SIZE_UNIT;
    return maximum & ~(size_t)7u;
}

bool p_heap_contains(const Pocket *p, const void *address) {
    uintptr_t first = (uintptr_t)p->heap;
    uintptr_t at = (uintptr_t)address;
    return p->heap != NULL && at >= first && at - first < p->heap_size;
}

static size_t bin_for(size_t size) {
    if (size <= P_SMALL_LIMIT) return size / P_SIZE_UNIT - P_MIN_BLOCK / P_SIZE_UNIT;
    return P_FREE_BINS - 1u;
}

static void link_free(Pocket *p, PObject *object) {
    size_t bin = bin_for(p_object_size(object));
    *(PObject **)((uint8_t *)object + sizeof(PObject)) = p->free_bins[bin];
    p->free_bins[bin] = object;
}

void p_heap_rebuild_bins(Pocket *p) {
    memset(p->free_bins, 0, sizeof(p->free_bins));
    size_t offset = 0u;
    while (offset < p->heap_size) {
        PObject *object = (PObject *)(p->heap + offset);
        size_t size = p_object_size(object);
        if (size < P_MIN_BLOCK || (size & 7u) != 0u || size > p->heap_size - offset)
            break;
        if (p_header_type(object) == P_FREE) link_free(p, object);
        offset += size;
    }
}

static bool remove_free(Pocket *p, PObject *target) {
    size_t bin = bin_for(p_object_size(target));
    PObject **link = &p->free_bins[bin];
    while (*link != NULL) {
        if (*link == target) {
            *link = *(PObject **)((uint8_t *)target + sizeof(PObject));
            return true;
        }
        link = (PObject **)((uint8_t *)*link + sizeof(PObject));
    }
    return false;
}

static PObject *find_free(Pocket *p, size_t bytes) {
    if (bytes <= P_SMALL_LIMIT) {
        size_t first = bin_for(bytes);
        size_t end = P_SMALL_LIMIT / P_SIZE_UNIT - P_MIN_BLOCK / P_SIZE_UNIT + 1u;
        for (size_t i = first; i < end; ++i) {
            for (PObject *object = p->free_bins[i]; object != NULL;
                 object = *(PObject **)((uint8_t *)object + sizeof(PObject)))
                if (p_object_size(object) >= bytes) return object;
        }
    }
    for (PObject *object = p->free_bins[P_FREE_BINS - 1u]; object != NULL;
         object = *(PObject **)((uint8_t *)object + sizeof(PObject)))
        if (p_object_size(object) >= bytes) return object;
    return NULL;
}

void p_clear_error(Pocket *p) {
    if (p == NULL) return;
    memset(&p->error, 0, sizeof(p->error));
    p->error.code = P_OK;
}

Value p_error(Pocket *p, PErrorCode code, const char *message) {
    if (p != NULL) {
        p->error.code = code;
        p->error.line = 0u;
        p->error.column = 0u;
        if (message == NULL) message = "error";
        size_t i = 0u;
        for (; i + 1u < sizeof(p->error.message) && message[i] != '\0'; ++i)
            p->error.message[i] = message[i];
        p->error.message[i] = '\0';
    }
    return 0;
}

static bool requestable(size_t bytes) {
    return bytes <= max_block_size();
}

void *p_alloc(Pocket *p, PType type, size_t bytes) {
    if (p == NULL || p->heap == NULL || type <= P_FREE || type > P_PACKAGE_ENTRY) {
        if (p != NULL) (void)p_error(p, P_ERR_HEAP, "invalid heap allocation");
        return NULL;
    }
    size_t size = align_block(bytes);
    if (size == 0u || !requestable(size)) {
        (void)p_error(p, P_ERR_RANGE, "object allocation is too large");
        return NULL;
    }
    PObject *block = find_free(p, size);
    if (block == NULL) {
        (void)p_error(p, P_ERR_HEAP, "heap exhausted; reserve at a safe point first");
        return NULL;
    }
    size_t available = p_object_size(block);
    if (!remove_free(p, block)) {
        (void)p_error(p, P_ERR_HEAP, "heap free-list invariant failed");
        return NULL;
    }
    if (available - size >= P_MIN_BLOCK) {
        PObject *remainder = (PObject *)((uint8_t *)block + size);
        remainder->header = p_header_make(P_FREE, available - size);
        link_free(p, remainder);
    } else {
        size = available;
    }
    block->header = p_header_make(type, size);
    p->live_bytes += size;
    p->allocated_since_gc += size;
    return block;
}

static bool has_room_after_collection(Pocket *p, size_t bytes) {
    return find_free(p, bytes) != NULL;
}

bool p_reserve(Pocket *p, size_t bytes) {
    if (p == NULL || p->heap == NULL) return false;
    size_t size = bytes == 0u ? P_MIN_BLOCK : align_block(bytes);
    if (size == 0u || !requestable(size)) {
        (void)p_error(p, P_ERR_RANGE, "reservation request is too large");
        return false;
    }
    if (p->initializing) return has_room_after_collection(p, size);
    bool room = has_room_after_collection(p, size);
    size_t threshold = p->heap_size / 4u;
    bool collect = p->gc_stress || !room || p->allocated_since_gc >= threshold;
    if (collect) p_gc(p);
    if (has_room_after_collection(p, size)) return true;
    (void)p_error(p, P_ERR_HEAP, "not enough contiguous heap space after collection");
    return false;
}

static void mark_value(Pocket *p, size_t *mark_count, Value value) {
    if (!p_is_pointer(value) || !p_heap_contains(p, (void *)value)) return;
    size_t offset = (size_t)((uint8_t *)value - p->heap);
    if ((offset & 7u) != 0u) return;
    PObject *object = (PObject *)value;
    size_t size = p_object_size(object);
    if (size < P_MIN_BLOCK || size > p->heap_size - offset ||
        p_header_type(object) == P_FREE || p_header_type(object) > P_PACKAGE_ENTRY)
        return;
    if ((object->header & P_HEADER_MARK) != 0u) return;
    object->header |= P_HEADER_MARK;
    if (*mark_count >= p->mark_capacity) return;
    p->mark_stack[(*mark_count)++] = value;
}

static void mark_children(Pocket *p, size_t *mark_count, PObject *object) {
    size_t object_size = p_object_size(object);
    switch (p_header_type(object)) {
    case P_CONS: {
        if (object_size < sizeof(PCons)) break;
        PCons *cell = (PCons *)object;
        mark_value(p, mark_count, cell->car); mark_value(p, mark_count, cell->cdr);
        break;
    }
    case P_VECTOR: {
        if (object_size < offsetof(PVector, data)) break;
        PVector *vector = (PVector *)object;
        size_t max = (p_object_size(object) - offsetof(PVector, data)) / sizeof(Value);
        size_t length = vector->length < max ? vector->length : max;
        for (size_t i = 0; i < length; ++i) mark_value(p, mark_count, vector->data[i]);
        break;
    }
    case P_SYMBOL: {
        if (object_size < sizeof(PSymbol)) break;
        PSymbol *symbol = (PSymbol *)object;
        mark_value(p, mark_count, symbol->name); mark_value(p, mark_count, symbol->home);
        mark_value(p, mark_count, symbol->value); mark_value(p, mark_count, symbol->function);
        mark_value(p, mark_count, symbol->plist);
        break;
    }
    case P_PACKAGE: {
        if (object_size < sizeof(PPackage)) break;
        PPackage *package = (PPackage *)object;
        mark_value(p, mark_count, package->name); mark_value(p, mark_count, package->nicknames);
        mark_value(p, mark_count, package->entries); mark_value(p, mark_count, package->entry_tail);
        mark_value(p, mark_count, package->uses); mark_value(p, mark_count, package->shadows);
        mark_value(p, mark_count, package->next);
        break;
    }
    case P_PACKAGE_ENTRY: {
        if (object_size < sizeof(PPackageEntry)) break;
        PPackageEntry *entry = (PPackageEntry *)object;
        mark_value(p, mark_count, entry->symbol); mark_value(p, mark_count, entry->next);
        break;
    }
    case P_RECORD: {
        if (object_size < offsetof(PRecord, data)) break;
        PRecord *record = (PRecord *)object;
        size_t max = (p_object_size(object) - offsetof(PRecord, data)) / sizeof(Value);
        size_t length = record->length < max ? record->length : max;
        mark_value(p, mark_count, record->type);
        for (size_t i = 0; i < length; ++i) mark_value(p, mark_count, record->data[i]);
        break;
    }
    default:
        /* Numeric and byte/string payloads contain no Values. New pointer
         * bearing object kinds must be added here before they are allocated. */
        break;
    }
}

void p_gc(Pocket *p) {
    if (p == NULL || p->heap == NULL || p->initializing) return;
    size_t mark_count = 0u;
    for (size_t i = 0; i < p->root_count; ++i) {
        PRoot *root = &p->roots[i];
        for (size_t j = 0; root->values != NULL && j < root->count; ++j)
            mark_value(p, &mark_count, root->values[j]);
    }
    mark_value(p, &mark_count, p->nil); mark_value(p, &mark_count, p->truth);
    mark_value(p, &mark_count, p->packages); mark_value(p, &mark_count, p->package_tail);
    mark_value(p, &mark_count, p->cl); mark_value(p, &mark_count, p->user);
    mark_value(p, &mark_count, p->keyword); mark_value(p, &mark_count, p->sys);
    mark_value(p, &mark_count, p->current_package); mark_value(p, &mark_count, p->features);
    mark_value(p, &mark_count, p->error_hook);
    mark_value(p, &mark_count, p->error.datum); mark_value(p, &mark_count, p->error.expected);
    while (mark_count != 0u) {
        Value value = p->mark_stack[--mark_count];
        mark_children(p, &mark_count, (PObject *)value);
    }

    p->live_bytes = 0u;
    size_t offset = 0u;
    PObject *previous_free = NULL;
    while (offset < p->heap_size) {
        PObject *object = (PObject *)(p->heap + offset);
        size_t size = p_object_size(object);
        if (size < P_MIN_BLOCK || (size & 7u) != 0u || size > p->heap_size - offset)
            break;
        PType type = p_header_type(object);
        if (type != P_FREE && (object->header & P_HEADER_MARK) == 0u) {
            object->header = p_header_make(P_FREE, size);
            type = P_FREE;
        } else if (type != P_FREE) {
            object->header &= ~P_HEADER_MARK;
            p->live_bytes += size;
        }
        if (type == P_FREE) {
            if (previous_free != NULL) {
                size_t prior_size = p_object_size(previous_free);
                if (size <= max_block_size() - prior_size)
                    previous_free->header = p_header_make(P_FREE, prior_size + size);
                else
                    previous_free = object;
            } else {
                previous_free = object;
            }
        } else {
            previous_free = NULL;
        }
        offset += size;
    }
    p_heap_rebuild_bins(p);
    p->allocated_since_gc = 0u;
    ++p->collections;
}

size_t p_root_save(const Pocket *p) {
    return p == NULL ? 0u : p->root_count;
}

bool p_roots(Pocket *p, Value *values, size_t count) {
    if (p == NULL || (count != 0u && values == NULL) ||
        p->root_count >= P_ROOT_LIMIT) {
        if (p != NULL) (void)p_error(p, P_ERR_STACK, "root stack exhausted");
        return false;
    }
    p->roots[p->root_count++] = (PRoot){ values, count };
    return true;
}

void p_root_restore(Pocket *p, size_t saved) {
    if (p == NULL) return;
    if (saved <= p->root_count) p->root_count = saved;
}

bool p_init(Pocket *p, size_t arena_size) {
    if (p == NULL) return false;
    memset(p, 0, sizeof(*p));
    p_clear_error(p);
#ifdef POCKET_GC_STRESS
    p->gc_stress = true;
#endif
    const size_t word = sizeof(Value);
    size_t capacity = arena_size / (P_MIN_BLOCK + word);
    if (capacity == 0u || capacity > SIZE_MAX / word) {
        (void)p_error(p, P_ERR_RANGE, "arena is too small");
        return false;
    }
    size_t heap_size = (arena_size - capacity * word) & ~(size_t)7u;
    while (heap_size / P_MIN_BLOCK < capacity) {
        if (capacity == 0u) break;
        --capacity;
        heap_size = (arena_size - capacity * word) & ~(size_t)7u;
    }
    if (heap_size < P_MIN_BLOCK || capacity == 0u) {
        (void)p_error(p, P_ERR_RANGE, "arena is too small");
        return false;
    }
    p->allocation = malloc(arena_size);
    if (p->allocation == NULL) {
        (void)p_error(p, P_ERR_HEAP, "unable to allocate arena");
        return false;
    }
    p->arena_size = arena_size;
    p->heap_size = heap_size;
    p->heap = (uint8_t *)p->allocation;
    p->mark_stack = (Value *)(p->heap + heap_size);
    p->mark_capacity = capacity;
    p->initializing = true;
    size_t offset = 0u;
    size_t block_limit = max_block_size();
    while (offset < heap_size) {
        size_t size = heap_size - offset;
        if (size > block_limit) size = block_limit;
        PObject *block = (PObject *)(p->heap + offset);
        block->header = p_header_make(P_FREE, size);
        offset += size;
    }
    p_heap_rebuild_bins(p);
    if (!p_packages_init(p)) {
        p->initializing = false;
        p_destroy(p);
        return false;
    }
    p->initializing = false;
    return true;
}

void p_destroy(Pocket *p) {
    if (p == NULL) return;
    free(p->allocation);
    memset(p, 0, sizeof(*p));
}
