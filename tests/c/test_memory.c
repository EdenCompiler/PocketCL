#include "pocket.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

void test_memory(void) {
    Pocket pocket;
    ASSERT(p_init(&pocket, 1024u * 1024u));
    ASSERT(p_fixnum_bits() == (sizeof(Value) == 4u ? 31u : 62u));
    ASSERT(p_fixnum_value(p_fixnum(p_fixnum_min())) == p_fixnum_min());
    ASSERT(p_fixnum_value(p_fixnum(p_fixnum_max())) == p_fixnum_max());
    ASSERT(p_checked_fixnum(&pocket, p_fixnum_max() + 1) == 0u);
    ASSERT(pocket.error.code == P_ERR_RANGE);
    p_clear_error(&pocket);

    size_t collections = pocket.collections;
    pocket.gc_stress = false;
    for (size_t i = 0; i < 4u; ++i) ASSERT(p_reserve(&pocket, 64u));
    ASSERT(pocket.collections == collections);

    Value cycle[1];
    cycle[0] = p_cons(&pocket, p_fixnum(7), 0u);
    ASSERT(cycle[0] != 0u);
    ASSERT(p_roots(&pocket, cycle, 1u));
    p_cons_ptr(cycle[0])->cdr = cycle[0];
    pocket.gc_stress = true;
    ASSERT(p_reserve(&pocket, 512u * 1024u));
    ASSERT(p_type(cycle[0]) == P_CONS);
    ASSERT(p_cons_ptr(cycle[0])->cdr == cycle[0]);

    Value first = p_vector(&pocket, 3u, p_fixnum(5));
    Value second = p_vector(&pocket, 3u, p_fixnum(5));
    ASSERT(first != 0u && second != 0u && p_equal(first, second));
    p_vector_ptr(first)->data[1] = cycle[0];
    p_vector_ptr(second)->data[1] = cycle[0];
    ASSERT(p_equal(first, second));

    Value wide = p_bytes(&pocket, 32768u);
    ASSERT(wide != 0u && p_type(wide) == P_BYTES);
    Value too_large = p_vector(&pocket, SIZE_MAX, 0u);
    ASSERT(too_large == 0u && pocket.error.code == P_ERR_RANGE);
    p_clear_error(&pocket);
    ASSERT(p_alloc(&pocket, P_BYTES, 2u * 1024u * 1024u) == NULL);
    ASSERT(pocket.error.code == (sizeof(Value) >= 8u ? P_ERR_HEAP : P_ERR_RANGE));
    p_clear_error(&pocket);

    const char encoded[] = "A\xe2\x82\xac\xf0\x9f\x8c\x99";
    Value string = p_string_utf8(&pocket, encoded, sizeof(encoded) - 1u);
    ASSERT(string != 0u && p_type(string) == P_STRING);
    char decoded[16];
    size_t written = 0u;
    ASSERT(p_string_to_utf8(&pocket, string, decoded, sizeof(decoded), &written));
    ASSERT(written == sizeof(encoded) - 1u);
    ASSERT(memcmp(decoded, encoded, written) == 0);
    ASSERT(p_string_equals_ascii(p_string_utf8(&pocket, "hello", 5u), "hello"));
    ASSERT(p_string_utf8(&pocket, "\xc0\x80", 2u) == 0u);
    ASSERT(pocket.error.code == P_ERR_READER);
    p_clear_error(&pocket);

    Value live[4] = { cycle[0], first, second, string };
    ASSERT(p_roots(&pocket, live, 4u));
    size_t before = pocket.live_bytes;
    Value garbage = p_bytes(&pocket, 4096u);
    ASSERT(garbage != 0u);
    ASSERT(pocket.live_bytes > before);
    ASSERT(p_reserve(&pocket, 512u * 1024u));
    ASSERT(pocket.live_bytes < before + 4096u);
    ASSERT(p_type(live[0]) == P_CONS && p_type(live[3]) == P_STRING);

    Value dropped_cycle = p_cons(&pocket, p_fixnum(9), 0u);
    ASSERT(dropped_cycle != 0u);
    p_cons_ptr(dropped_cycle)->cdr = dropped_cycle;
    size_t retained = pocket.live_bytes;
    ASSERT(p_reserve(&pocket, 512u * 1024u));
    ASSERT(pocket.live_bytes < retained + 16u);

    /* Churn alternating live and dead blocks to exercise free-list rebuilding,
     * then confirm adjacent dead blocks combine into a useful large block. */
    Value anchors[8];
    for (size_t i = 0; i < 8u; ++i) {
        anchors[i] = p_bytes(&pocket, 512u);
        ASSERT(anchors[i] != 0u);
        ASSERT(p_bytes(&pocket, 512u) != 0u);
    }
    ASSERT(p_roots(&pocket, anchors, 8u));
    ASSERT(p_reserve(&pocket, 256u * 1024u));
    for (size_t i = 0; i < 8u; ++i) ASSERT(p_type(anchors[i]) == P_BYTES);
    ASSERT(p_reserve(&pocket, 128u * 1024u));

    ASSERT(p_alloc(&pocket, P_BYTES, SIZE_MAX) == NULL);
    ASSERT(pocket.error.code == P_ERR_RANGE);
    p_clear_error(&pocket);
    while (p_alloc(&pocket, P_BYTES, 4096u) != NULL) { }
    ASSERT(pocket.error.code == P_ERR_HEAP);
    p_root_restore(&pocket, 0u);
    p_destroy(&pocket);
}
