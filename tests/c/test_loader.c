#include "test.h"

#include "loader.h"
#include "eval.h"

#include <string.h>
#include <stdlib.h>

/* A tiny verified entry proto that returns pool[0]. Its pool contains a
 * self-referential constant cons plus an inherited CL-USER::CAR identity,
 * exercising graph materialization and shadowing without execution. */
enum { LARGE_STRING_BYTES = 4096u };
typedef struct { uint8_t data[8192]; size_t length; } ImageBuffer;

static void put_u8(ImageBuffer *b, uint8_t value)
{
    b->data[b->length++] = value;
}

static void put_u32(ImageBuffer *b, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i)
        put_u8(b, (uint8_t)(value >> (i * 8u)));
}

static size_t begin_section(ImageBuffer *b, uint32_t id)
{
    put_u32(b, id);
    {
        size_t length_offset = b->length;
        put_u32(b, 0u);
        return length_offset;
    }
}

static void finish_section(ImageBuffer *b, size_t length_offset)
{
    size_t length = b->length - length_offset - 4u;
    for (unsigned i = 0u; i < 4u; ++i)
        b->data[length_offset + i] = (uint8_t)(length >> (i * 8u));
}

static void patch_u32(uint8_t *data, size_t offset, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i)
        data[offset + i] = (uint8_t)(value >> (i * 8u));
}

/* Expand the final string constant without putting a megabyte on the stack.
 * The section lengths and top-level image size are updated with the payload. */
static uint8_t *make_large_string_image(const ImageBuffer *small,
                                       size_t string_bytes,
                                       size_t *image_length)
{
    size_t cursor = 16u, constants_start = 0u, constants_end = 0u;
    size_t payload_length_offset = 0u, payload_start = 0u;
    uint32_t old_length = 0u;
    uint8_t *data;
    size_t extra, new_length;

    for (uint32_t section = 1u; section <= 4u; ++section) {
        uint32_t id = (uint32_t)small->data[cursor] |
                      ((uint32_t)small->data[cursor + 1u] << 8u) |
                      ((uint32_t)small->data[cursor + 2u] << 16u) |
                      ((uint32_t)small->data[cursor + 3u] << 24u);
        uint32_t length = (uint32_t)small->data[cursor + 4u] |
                          ((uint32_t)small->data[cursor + 5u] << 8u) |
                          ((uint32_t)small->data[cursor + 6u] << 16u) |
                          ((uint32_t)small->data[cursor + 7u] << 24u);
        if (id == 2u) {
            constants_start = cursor + 8u;
            constants_end = constants_start + length;
            break;
        }
        cursor += 8u + length;
    }
    if (constants_start == 0u || constants_end > small->length ||
        constants_end - constants_start < 4u)
        return NULL;
    cursor = constants_start + 4u;
    while (cursor < constants_end) {
        uint8_t tag = small->data[cursor++];
        uint32_t length = (uint32_t)small->data[cursor] |
                          ((uint32_t)small->data[cursor + 1u] << 8u) |
                          ((uint32_t)small->data[cursor + 2u] << 16u) |
                          ((uint32_t)small->data[cursor + 3u] << 24u);
        size_t length_offset = cursor;
        cursor += 4u;
        if ((size_t)length > constants_end - cursor)
            return NULL;
        if (tag == 4u) {
            payload_length_offset = length_offset;
            payload_start = cursor;
            old_length = length;
        }
        cursor += length;
    }
    if (cursor != constants_end || payload_start == 0u ||
        string_bytes < old_length || string_bytes > UINT32_MAX)
        return NULL;
    extra = string_bytes - old_length;
    if (extra > SIZE_MAX - small->length)
        return NULL;
    new_length = small->length + extra;
    data = (uint8_t *)malloc(new_length);
    if (data == NULL)
        return NULL;
    memcpy(data, small->data, payload_start);
    memset(data + payload_start, 'x', string_bytes);
    memcpy(data + payload_start + string_bytes,
           small->data + payload_start + old_length,
           small->length - payload_start - old_length);
    patch_u32(data, payload_length_offset, (uint32_t)string_bytes);
    patch_u32(data, 8u, (uint32_t)new_length);
    {
        size_t section_header = constants_start - 8u;
        uint32_t section_length = (uint32_t)constants_end -
                                  (uint32_t)constants_start;
        patch_u32(data, section_header + 4u,
                  section_length + (uint32_t)extra);
    }
    *image_length = new_length;
    return data;
}

static ImageBuffer make_valid_image(int64_t fixnum)
{
    ImageBuffer b = {{0u}, 0u};
    size_t section;
    memcpy(b.data, "PFAS", 4u);
    b.length = 4u;
    put_u8(&b, 1u); put_u8(&b, 0u);       /* format version */
    put_u8(&b, 0u);
    put_u8(&b, p_fixnum_bits() == 31u ? 1u : 0u); /* target flag */
    put_u32(&b, 0u);                       /* total size, patched below */
    put_u32(&b, 4u);

    section = begin_section(&b, 1u);
    put_u32(&b, 1u);                       /* symbols */
    put_u32(&b, 7u);                       /* CL-USER */
    memcpy(b.data + b.length, "CL-USER", 7u); b.length += 7u;
    put_u32(&b, 3u);                       /* CAR is inherited from CL */
    memcpy(b.data + b.length, "CAR", 3u); b.length += 3u;
    finish_section(&b, section);

    section = begin_section(&b, 2u);
    put_u32(&b, 6u);                       /* constants */
    put_u8(&b, 5u); put_u32(&b, 4u); put_u32(&b, 0u); /* symbol ref */
    put_u8(&b, 7u); put_u32(&b, 16u); put_u32(&b, 3u);
    put_u32(&b, 3u); put_u32(&b, 0u); put_u32(&b, 4u); /* pool */
    put_u8(&b, 8u); put_u32(&b, 6u);       /* code: CONST 0; RETURN */
    put_u8(&b, 1u); put_u32(&b, 0u); put_u8(&b, 14u);
    put_u8(&b, 6u); put_u32(&b, 8u);       /* self-referential cons */
    put_u32(&b, 3u); put_u32(&b, 3u);
    put_u8(&b, 1u); put_u32(&b, 8u);       /* target fixnum candidate */
    for (unsigned i = 0u; i < 8u; ++i)
        put_u8(&b, (uint8_t)((uint64_t)fixnum >> (i * 8u)));
    put_u8(&b, 4u); put_u32(&b, LARGE_STRING_BYTES);
    memset(b.data + b.length, 'x', LARGE_STRING_BYTES);
    b.length += LARGE_STRING_BYTES;         /* bounded large allocation */
    finish_section(&b, section);

    section = begin_section(&b, 3u);
    put_u32(&b, 1u);
    put_u32(&b, 0u);                       /* name const */
    put_u32(&b, 0u); put_u32(&b, 0u);      /* required, rest */
    put_u32(&b, 0u); put_u32(&b, 0u);      /* locals, free */
    put_u32(&b, 1u); put_u32(&b, p_fixnum_bits()); /* max stack, target */
    put_u32(&b, 2u); put_u32(&b, 1u);      /* code, pool */
    finish_section(&b, section);

    section = begin_section(&b, 4u);
    put_u32(&b, 1u); put_u32(&b, 0u);
    finish_section(&b, section);

    for (unsigned i = 0u; i < 4u; ++i)
        b.data[8u + i] = (uint8_t)(b.length >> (i * 8u));
    return b;
}

static Value load_image(Pocket *p, const ImageBuffer *image)
{
    size_t saved = p_root_save(p);
    Value bytes = p_bytes(p, image->length);
    Value closure;
    size_t reserve = 0u;
    ASSERT(bytes != 0u);
    memcpy(((PBytes *)bytes)->data, image->data, image->length);
    ASSERT(p_roots(p, &bytes, 1u));
    ASSERT(p_fasl_required_bytes(p, bytes, &reserve));
    ASSERT(reserve > LARGE_STRING_BYTES);
    ASSERT(p_reserve(p, reserve));
    closure = p_fasl_load(p, bytes);
    p_root_restore(p, saved);
    return closure;
}

static void expect_bad_image(Pocket *p, ImageBuffer image)
{
    Value bytes = p_bytes(p, image.length);
    size_t reserve = 0u;
    size_t saved = p_root_save(p);
    ASSERT(bytes != 0u);
    memcpy(((PBytes *)bytes)->data, image.data, image.length);
    ASSERT(p_roots(p, &bytes, 1u));
    p_clear_error(p);
    if (p_fasl_required_bytes(p, bytes, &reserve)) {
        ASSERT(p_reserve(p, reserve));
        (void)p_fasl_load(p, bytes);
    }
    ASSERT(p->error.code == P_ERR_IMAGE || p->error.code == P_ERR_RANGE);
    p_root_restore(p, saved);
}

void test_loader(void)
{
    Pocket p;
    PState state;
    ImageBuffer image = make_valid_image(42);
    Value closure, proto_value, pool_value, cycle, helper_name, load_name;
    size_t roots, reserve;

    ASSERT(p_init(&p, 1024u * 1024u));
    ASSERT(p_reserve(&p, 128u * 1024u));
    p_eval_init(&state, &p);
    ASSERT(p_install_fasl_primitives(&state));
    load_name = p_find_symbol(&p, p.sys, "%LOAD-FASL", NULL);
    ASSERT(load_name != p.nil && p_type(p_symbol_ptr(load_name)->function) == P_PRIMITIVE);
    helper_name = p_find_symbol(&p, p.sys, "%FLONUM-BYTES", NULL);
    ASSERT(helper_name != p.nil && p_type(p_symbol_ptr(helper_name)->function) == P_PRIMITIVE);
    {
        PPrimitive *helper = (PPrimitive *)p_symbol_ptr(helper_name)->function;
        Value argument;
        Value encoded;
        size_t helper_roots = p_root_save(&p);
        ASSERT(helper->reserve != NULL);
        ASSERT(p_reserve(&p, sizeof(PFlonum) + 64u));
        argument = p_flonum(&p, 1.5);
        ASSERT(argument != 0u && p_roots(&p, &argument, 1u));
        ASSERT(helper->reserve(&state, 1u, &argument, &reserve));
        ASSERT(p_reserve(&p, reserve));
        encoded = helper->entry(&state, 1u, &argument);
        ASSERT(p_type(encoded) == P_BYTES && ((PBytes *)encoded)->length == 8u);
        ASSERT(((PBytes *)encoded)->data[0] == 0u &&
               ((PBytes *)encoded)->data[1] == 0u &&
               ((PBytes *)encoded)->data[2] == 0u &&
               ((PBytes *)encoded)->data[3] == 0u &&
               ((PBytes *)encoded)->data[4] == 0u &&
               ((PBytes *)encoded)->data[5] == 0u &&
               ((PBytes *)encoded)->data[6] == 0xf8u &&
               ((PBytes *)encoded)->data[7] == 0x3fu);
        p_root_restore(&p, helper_roots);
    }
    roots = p_root_save(&p);
    {
        PPrimitive *loader = (PPrimitive *)p_symbol_ptr(load_name)->function;
        Value bytes = p_bytes(&p, image.length);
        Value argument[1];
        size_t loader_roots = p_root_save(&p);
        ASSERT(bytes != 0u);
        memcpy(((PBytes *)bytes)->data, image.data, image.length);
        ASSERT(p_roots(&p, &bytes, 1u));
        ASSERT(loader->reserve(&state, 1u, &bytes, &reserve));
        ASSERT(p_reserve(&p, reserve));
        argument[0] = bytes;
        closure = loader->entry(&state, 1u, argument);
        ASSERT(closure != 0u && p_type(closure) == P_CLOSURE);
        p_root_restore(&p, loader_roots);
    }
    closure = load_image(&p, &image);
    ASSERT(closure != 0u && p_type(closure) == P_CLOSURE);
    ASSERT((((PClosure *)closure)->flags & P_EVAL_COMPILED) != 0u);
    proto_value = ((PClosure *)closure)->body;
    ASSERT(p_type(proto_value) == P_PROTO);
    ASSERT((((PProto *)proto_value)->flags & P_PROTO_VERIFIED) != 0u);
    pool_value = ((PProto *)proto_value)->constants;
    ASSERT(p_type(pool_value) == P_VECTOR);
    cycle = p_vector_ptr(pool_value)->data[0];
    ASSERT(p_type(cycle) == P_CONS);
    ASSERT(p_cons_ptr(cycle)->car == cycle && p_cons_ptr(cycle)->cdr == cycle);
    ASSERT(p_type(p_vector_ptr(pool_value)->data[1]) == P_SYMBOL);
    ASSERT(p_symbol_ptr(p_vector_ptr(pool_value)->data[1])->home == p.user);
    ASSERT(p_is_fixnum(p_vector_ptr(pool_value)->data[2]));
    ASSERT(p_fixnum_value(p_vector_ptr(pool_value)->data[2]) == 42);
    p_root_restore(&p, roots);

    /* PFAS stores signed 64-bit values, while the running VM may use
     * 31-bit or 62-bit fixnums. Both exact target endpoints load unchanged. */
    image = make_valid_image(p_fixnum_min());
    closure = load_image(&p, &image);
    ASSERT(closure != 0u && p_type(closure) == P_CLOSURE);
    pool_value = ((PProto *)((PClosure *)closure)->body)->constants;
    ASSERT(p_fixnum_value(p_vector_ptr(pool_value)->data[2]) == p_fixnum_min());
    image = make_valid_image(p_fixnum_max());
    closure = load_image(&p, &image);
    ASSERT(closure != 0u && p_type(closure) == P_CLOSURE);
    pool_value = ((PProto *)((PClosure *)closure)->body)->constants;
    ASSERT(p_fixnum_value(p_vector_ptr(pool_value)->data[2]) == p_fixnum_max());

    image = make_valid_image(p_fixnum_min() - 1);
    expect_bad_image(&p, image);
    image = make_valid_image(p_fixnum_max() + 1);
    expect_bad_image(&p, image);
    image = make_valid_image(INT64_MIN);
    expect_bad_image(&p, image);
    image = make_valid_image(INT64_MAX);
    expect_bad_image(&p, image);

    image.data[0] = 'X';
    expect_bad_image(&p, image);
    image = make_valid_image(42);
    image.data[4] = 2u;                     /* unsupported version */
    expect_bad_image(&p, image);
    image = make_valid_image(42);
    image.data[7] = p_fixnum_bits() == 31u ? 0u : 1u;
    expect_bad_image(&p, image);
    image = make_valid_image(42);
    image.data[image.length - 1u] = 1u;     /* invalid entry proto */
    expect_bad_image(&p, image);
    image = make_valid_image(42);
    image.data[57u] = 0xffu;                /* invalid symbol reference */
    expect_bad_image(&p, image);
    image = make_valid_image(42);
    image.data[43u] = 0xffu;                /* malformed symbol-name UTF-8 */
    expect_bad_image(&p, image);
    image = make_valid_image(42);
    image.data[43u] = 0u;                    /* C API names cannot contain NUL */
    expect_bad_image(&p, image);

    /* The serialized input fits, but materializing its UTF-8 bytes as VM
     * character storage cannot fit alongside that input in this 2 MiB heap. */
    {
        enum { EXHAUSTION_STRING_BYTES = 384u * 1024u };
        Pocket constrained;
        ImageBuffer small = make_valid_image(42);
        uint8_t *large_data;
        size_t large_length, saved, required = 0u;
        Value large_bytes;
        Value packages_before;
        large_data = make_large_string_image(&small, EXHAUSTION_STRING_BYTES,
                                             &large_length);
        ASSERT(large_data != NULL);
        ASSERT(large_length < 1024u * 1024u);
        ASSERT(p_init(&constrained, 2u * 1024u * 1024u));
        ASSERT(p_reserve(&constrained, offsetof(PBytes, data) + large_length));
        large_bytes = p_bytes(&constrained, large_length);
        ASSERT(large_bytes != 0u);
        memcpy(((PBytes *)large_bytes)->data, large_data, large_length);
        free(large_data);
        saved = p_root_save(&constrained);
        ASSERT(p_roots(&constrained, &large_bytes, 1u));
        packages_before = constrained.packages;
        ASSERT(p_fasl_required_bytes(&constrained, large_bytes, &required));
        p_clear_error(&constrained);
        ASSERT(!p_reserve(&constrained, required));
        ASSERT(constrained.error.code == P_ERR_HEAP);
        ASSERT(constrained.packages == packages_before);
        p_root_restore(&constrained, saved);
        p_destroy(&constrained);
    }
    p_eval_destroy(&state);
    p_destroy(&p);
}
