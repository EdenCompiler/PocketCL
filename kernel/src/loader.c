#include "loader.h"

#include <float.h>
#include <limits.h>
#include <string.h>

/* PFAS is deliberately parsed from byte slices. The file is an interchange
 * format, not a dump of C structs: every integer is decoded little-endian and
 * every extent is checked before a pointer is formed. */
enum {
    FASL_HEADER_SIZE = 16u,
    FASL_SECTION_HEADER_SIZE = 8u,
    FASL_SECTION_SYMBOLS = 1u,
    FASL_SECTION_CONSTANTS = 2u,
    FASL_SECTION_PROTOS = 3u,
    FASL_SECTION_TOPLEVEL = 4u,
    FASL_SECTION_COUNT = 4u,
    FASL_CONSTANT_FIXNUM = 1u,
    FASL_CONSTANT_CHARACTER = 2u,
    FASL_CONSTANT_FLONUM = 3u,
    FASL_CONSTANT_STRING = 4u,
    FASL_CONSTANT_SYMBOL = 5u,
    FASL_CONSTANT_CONS = 6u,
    FASL_CONSTANT_VECTOR = 7u,
    FASL_CONSTANT_BYTES = 8u,
    FASL_CONSTANT_PROTO = 9u,
    FASL_PROTO_FIELD_COUNT = 9u
};

#define FASL_SYMBOL_UNINTERNED UINT32_MAX

typedef struct {
    uint32_t package_length;
    uint32_t name_length;
    size_t package_offset;
    size_t name_offset;
} FASLSymbol;

typedef struct {
    uint8_t tag;
    uint32_t payload_length;
    size_t payload_offset;
} FASLConstant;

typedef struct {
    uint32_t field[FASL_PROTO_FIELD_COUNT];
} FASLProto;

typedef struct {
    size_t offset;
    size_t length;
    uint32_t count;
} FASLSection;

typedef struct {
    const uint8_t *data;
    size_t length;
    FASLSection sections[FASL_SECTION_COUNT];
    uint32_t flags;
    uint32_t symbol_count;
    uint32_t constant_count;
    uint32_t proto_count;
    uint32_t entry_proto;
    size_t max_name_bytes;
    size_t scratch_bytes;
    size_t verify_bytes;
    size_t required_bytes;
} FASLImage;

static bool image_error(Pocket *p, const char *message)
{
    (void)p_error(p, P_ERR_IMAGE, message);
    return false;
}

static bool checked_add_size(size_t a, size_t b, size_t *result)
{
    if (b > SIZE_MAX - a)
        return false;
    *result = a + b;
    return true;
}

static bool checked_mul_size(size_t a, size_t b, size_t *result)
{
    if (a != 0u && b > SIZE_MAX / a)
        return false;
    *result = a * b;
    return true;
}

static bool read_u32(const uint8_t *data, size_t limit, size_t *offset,
                     uint32_t *value)
{
    if (*offset > limit || limit - *offset < 4u)
        return false;
    *value = (uint32_t)data[*offset] |
             ((uint32_t)data[*offset + 1u] << 8u) |
             ((uint32_t)data[*offset + 2u] << 16u) |
             ((uint32_t)data[*offset + 3u] << 24u);
    *offset += 4u;
    return true;
}

static bool valid_utf8(const uint8_t *text, size_t length, bool reject_nul)
{
    size_t i = 0u;
    while (i < length) {
        uint8_t a = text[i++];
        uint32_t cp;
        unsigned trailing;
        if (a < 0x80u) {
            if (reject_nul && a == 0u) return false;
            continue;
        }
        if (a >= 0xc2u && a <= 0xdfu) {
            cp = (uint32_t)(a & 0x1fu);
            trailing = 1u;
        } else if (a >= 0xe0u && a <= 0xefu) {
            cp = (uint32_t)(a & 0x0fu);
            trailing = 2u;
        } else if (a >= 0xf0u && a <= 0xf4u) {
            cp = (uint32_t)(a & 0x07u);
            trailing = 3u;
        } else {
            return false;
        }
        if ((size_t)trailing > length - i)
            return false;
        for (unsigned j = 0u; j < trailing; ++j) {
            uint8_t b = text[i++];
            if ((b & 0xc0u) != 0x80u)
                return false;
            cp = (cp << 6u) | (uint32_t)(b & 0x3fu);
        }
        if ((trailing == 1u && cp < 0x80u) ||
            (trailing == 2u && cp < 0x800u) ||
            (trailing == 3u && cp < 0x10000u) ||
            cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
            return false;
    }
    return true;
}

static bool aligned_object_size(size_t payload, size_t *size)
{
    size_t rounded;
    if (payload > SIZE_MAX - 7u)
        return false;
    rounded = (payload + 7u) & ~(size_t)7u;
    if (rounded < 2u * sizeof(uintptr_t))
        rounded = 2u * sizeof(uintptr_t);
    *size = rounded;
    return true;
}

static bool add_object_budget(size_t payload, size_t *total)
{
    size_t object_size;
    return aligned_object_size(payload, &object_size) &&
           checked_add_size(*total, object_size, total);
}

static bool scan_symbols(Pocket *p, FASLImage *image, FASLSymbol *records,
                         size_t *allocations)
{
    FASLSection *section = &image->sections[0];
    size_t cursor = section->offset, end = section->offset + section->length;
    uint32_t count;
    if (!read_u32(image->data, end, &cursor, &count))
        return image_error(p, "missing FASL symbol count");
    section->count = count;
    image->symbol_count = count;
    for (uint32_t i = 0u; i < count; ++i) {
        uint32_t package_length, name_length;
        size_t package_offset = cursor, name_offset;
        if (!read_u32(image->data, end, &cursor, &package_length))
            return image_error(p, "truncated FASL symbol package name");
        if (package_length != FASL_SYMBOL_UNINTERNED) {
            if ((size_t)package_length > end - cursor ||
                !valid_utf8(image->data + cursor, package_length, true))
                return image_error(p, "invalid FASL symbol package name");
            package_offset = cursor;
            cursor += package_length;
            if ((size_t)package_length > image->max_name_bytes)
                image->max_name_bytes = package_length;
        {
            size_t name_payload;
            if (!checked_mul_size((size_t)package_length, sizeof(uint32_t),
                                  &name_payload) ||
                !checked_add_size(offsetof(PString, data), name_payload,
                                  &name_payload) ||
                !add_object_budget(name_payload, allocations) ||
            !add_object_budget(sizeof(PPackage), allocations))
                return image_error(p, "FASL package allocation estimate overflow");
        }
        }
        if (!read_u32(image->data, end, &cursor, &name_length) ||
            (size_t)name_length > end - cursor ||
            !valid_utf8(image->data + cursor, name_length, true))
            return image_error(p, "invalid FASL symbol name");
        name_offset = cursor;
        cursor += name_length;
        if ((size_t)name_length > image->max_name_bytes)
            image->max_name_bytes = name_length;
        if (records != NULL) {
            records[i].package_length = package_length;
            records[i].name_length = name_length;
            records[i].package_offset = package_offset;
            records[i].name_offset = name_offset;
        }
        {
            size_t name_payload;
            if (!checked_mul_size((size_t)name_length, sizeof(uint32_t),
                                  &name_payload) ||
                !checked_add_size(offsetof(PString, data), name_payload,
                                  &name_payload) ||
                !add_object_budget(name_payload, allocations) ||
            !add_object_budget(sizeof(PSymbol), allocations) ||
                !add_object_budget(sizeof(PPackageEntry), allocations) ||
                !add_object_budget(sizeof(PCons), allocations))
                return image_error(p, "FASL symbol allocation estimate overflow");
            /* Interned symbols are first represented by harmless uninterned
             * skeletons so the VM verifier can inspect constants before the
             * loader mutates any package. The final interned symbol and name
             * are a second pair of objects. */
            if (package_length != FASL_SYMBOL_UNINTERNED &&
                (!add_object_budget(name_payload, allocations) ||
                 !add_object_budget(sizeof(PSymbol), allocations)))
                return image_error(p, "FASL symbol skeleton estimate overflow");
        }
    }
    if (cursor != end)
        return image_error(p, "extra bytes in FASL symbol section");
    return true;
}

static bool read_constant_records(Pocket *p, FASLImage *image,
                                  FASLConstant *records, size_t *allocations,
                                  size_t *largest_byte_string)
{
    FASLSection *section = &image->sections[1];
    size_t cursor = section->offset, end = section->offset + section->length;
    uint32_t count;
    if (!read_u32(image->data, end, &cursor, &count))
        return image_error(p, "missing FASL constant count");
    section->count = count;
    image->constant_count = count;
    for (uint32_t i = 0u; i < count; ++i) {
        uint8_t tag;
        uint32_t payload_length;
        size_t payload_offset;
        if (cursor >= end)
            return image_error(p, "truncated FASL constant tag");
        tag = image->data[cursor++];
        if (!read_u32(image->data, end, &cursor, &payload_length) ||
            (size_t)payload_length > end - cursor)
            return image_error(p, "truncated FASL constant payload");
        payload_offset = cursor;
        if (records != NULL) {
            records[i].tag = tag;
            records[i].payload_length = payload_length;
            records[i].payload_offset = payload_offset;
        }
        switch (tag) {
        case FASL_CONSTANT_FIXNUM:
            if (payload_length != 8u)
                return image_error(p, "invalid FASL fixnum length");
            break;
        case FASL_CONSTANT_CHARACTER:
            if (payload_length != 4u)
                return image_error(p, "invalid FASL character length");
            break;
        case FASL_CONSTANT_FLONUM:
            if (payload_length != 8u)
                return image_error(p, "invalid FASL flonum length");
            if (!add_object_budget(sizeof(PFlonum), allocations))
                return image_error(p, "FASL flonum estimate overflow");
            break;
        case FASL_CONSTANT_STRING:
            if (!valid_utf8(image->data + cursor, payload_length, false))
                return image_error(p, "invalid UTF-8 FASL string");
            {
                size_t string_payload;
                if (!checked_mul_size((size_t)payload_length, sizeof(uint32_t),
                                      &string_payload) ||
                    !checked_add_size(offsetof(PString, data), string_payload,
                                      &string_payload) ||
                    !add_object_budget(string_payload, allocations))
                return image_error(p, "FASL string estimate overflow");
            }
            break;
        case FASL_CONSTANT_SYMBOL:
        case FASL_CONSTANT_PROTO:
            if (payload_length != 4u)
                return image_error(p, "invalid FASL reference length");
            break;
        case FASL_CONSTANT_CONS:
            if (payload_length != 8u ||
                !add_object_budget(sizeof(PCons), allocations))
                return image_error(p, "invalid FASL cons constant");
            break;
        case FASL_CONSTANT_VECTOR: {
            size_t vector_cursor = cursor;
            uint32_t vector_length;
            size_t refs_bytes, payload_bytes;
            if (!read_u32(image->data, cursor + payload_length,
                          &vector_cursor, &vector_length) ||
                !checked_mul_size(vector_length, 4u, &refs_bytes) ||
                refs_bytes != payload_length - 4u ||
                !checked_mul_size(vector_length, sizeof(Value), &payload_bytes) ||
                !checked_add_size(offsetof(PVector, data), payload_bytes,
                                 &payload_bytes) ||
                !add_object_budget(payload_bytes, allocations))
                return image_error(p, "invalid FASL vector constant");
            break;
        }
        case FASL_CONSTANT_BYTES:
            if ((size_t)payload_length > *largest_byte_string)
                *largest_byte_string = payload_length;
            {
                size_t byte_payload;
                if (!checked_add_size(offsetof(PBytes, data), payload_length,
                                      &byte_payload) ||
                    !add_object_budget(byte_payload, allocations))
                    return image_error(p, "FASL byte string estimate overflow");
            }
            break;
        default:
            return image_error(p, "unknown FASL constant tag");
        }
        cursor += payload_length;
    }
    if (cursor != end)
        return image_error(p, "extra bytes in FASL constant section");
    return true;
}

static bool scan_protos(Pocket *p, FASLImage *image, FASLProto *records,
                        size_t *allocations)
{
    FASLSection *section = &image->sections[2];
    size_t cursor = section->offset, end = section->offset + section->length;
    uint32_t count;
    if (!read_u32(image->data, end, &cursor, &count))
        return image_error(p, "missing FASL proto count");
    section->count = count;
    image->proto_count = count;
    {
        size_t proto_bytes;
        if (!checked_mul_size((size_t)count,
                              FASL_PROTO_FIELD_COUNT * 4u, &proto_bytes) ||
            proto_bytes != end - cursor)
            return image_error(p, "invalid FASL proto section size");
    }
    for (uint32_t i = 0u; i < count; ++i) {
        FASLProto proto;
        for (size_t j = 0u; j < FASL_PROTO_FIELD_COUNT; ++j) {
            if (!read_u32(image->data, end, &cursor, &proto.field[j]))
                return image_error(p, "truncated FASL proto record");
        }
        if (records != NULL)
            records[i] = proto;
        if (!add_object_budget(sizeof(PProto), allocations))
            return image_error(p, "FASL proto allocation estimate overflow");
    }
    return true;
}

static bool scan_image(Pocket *p, Value bytes, FASLImage *image,
                       FASLSymbol *symbols, FASLConstant *constants,
                       FASLProto *protos, bool estimate)
{
    PBytes *input;
    size_t cursor, allocations = 0u, largest_bytes = 0u;
    uint16_t version, flags;
    uint32_t total_length, section_count;
    if (p_type(bytes) != P_BYTES)
        return image_error(p, "FASL input must be a byte vector");
    input = (PBytes *)bytes;
    if (input->length < FASL_HEADER_SIZE)
        return image_error(p, "truncated PFAS header");
    image->data = input->data;
    image->length = input->length;
    if (memcmp(input->data, "PFAS", 4u) != 0)
        return image_error(p, "invalid PFAS magic");
    version = (uint16_t)((uint16_t)input->data[4] |
                         ((uint16_t)input->data[5] << 8u));
    flags = (uint16_t)((uint16_t)input->data[6] |
                       ((uint16_t)input->data[7] << 8u));
    if (version != P_FASL_VERSION)
        return image_error(p, "unsupported PFAS version");
    if ((flags & (uint16_t)~(P_FASL_COMPILER | P_FASL_DEBUG |
                             P_FASL_FIXNUM31)) != 0u)
        return image_error(p, "unknown PFAS flags");
    if (((flags & P_FASL_FIXNUM31) != 0u && p_fixnum_bits() != 31u) ||
        ((flags & P_FASL_FIXNUM31) == 0u && p_fixnum_bits() != 62u))
        return image_error(p, "PFAS target fixnum width does not match this runtime");
    cursor = 8u;
    if (!read_u32(input->data, input->length, &cursor, &total_length) ||
        !read_u32(input->data, input->length, &cursor, &section_count) ||
        (size_t)total_length != input->length || section_count != FASL_SECTION_COUNT)
        return image_error(p, "invalid PFAS total length or section count");
    memset(image->sections, 0, sizeof(image->sections));
    image->flags = flags;
    for (uint32_t i = 0u; i < FASL_SECTION_COUNT; ++i) {
        uint32_t section_id, section_length;
        size_t end;
        if (!read_u32(input->data, input->length, &cursor, &section_id) ||
            !read_u32(input->data, input->length, &cursor, &section_length) ||
            section_id != i + 1u ||
            !checked_add_size(cursor, section_length, &end) ||
            end > input->length)
            return image_error(p, "invalid PFAS section order or length");
        image->sections[i].offset = cursor;
        image->sections[i].length = section_length;
        cursor = end;
    }
    if (cursor != input->length)
        return image_error(p, "trailing bytes after PFAS sections");
    if (estimate) {
        image->symbol_count = image->constant_count = image->proto_count = 0u;
        image->max_name_bytes = 0u;
        if (!scan_symbols(p, image, symbols, &allocations) ||
            !read_constant_records(p, image, constants, &allocations,
                                   &largest_bytes) ||
            !scan_protos(p, image, protos, &allocations))
            return false;
        if (!add_object_budget(sizeof(PVector), &allocations) ||
            !add_object_budget(sizeof(PClosure), &allocations))
            return image_error(p, "FASL entry closure estimate overflow");
        /* One scratch object holds parsed records and identity/value tables.
         * Another holds reusable verifier work arrays; no host allocation or
         * collection occurs while p_fasl_load is constructing the image. */
        {
            size_t scratch = 0u, amount;
            if (!checked_mul_size(image->symbol_count, sizeof(FASLSymbol), &amount) ||
                !checked_add_size(scratch, amount, &scratch) ||
                !checked_mul_size(image->constant_count, sizeof(FASLConstant), &amount) ||
                !checked_add_size(scratch, amount, &scratch) ||
                !checked_mul_size(image->proto_count, sizeof(FASLProto), &amount) ||
                !checked_add_size(scratch, amount, &scratch) ||
                !checked_mul_size(image->symbol_count, sizeof(Value), &amount) ||
                !checked_add_size(scratch, amount, &scratch) ||
                !checked_mul_size(image->constant_count, sizeof(Value), &amount) ||
                !checked_add_size(scratch, amount, &scratch) ||
                !checked_mul_size(image->proto_count, sizeof(Value), &amount) ||
                !checked_add_size(scratch, amount, &scratch) ||
                !checked_add_size(scratch, 7u * sizeof(uintptr_t), &scratch))
                return image_error(p, "FASL scratch size overflow");
            if (!checked_add_size(image->max_name_bytes, 1u, &amount) ||
                !checked_add_size(scratch, amount, &scratch))
                return image_error(p, "FASL scratch size overflow");
            image->scratch_bytes = scratch;
            if (!checked_add_size(offsetof(PBytes, data), scratch, &amount) ||
                !add_object_budget(amount, &allocations))
                return image_error(p, "FASL scratch allocation overflow");
        }
        if (largest_bytes != 0u) {
            size_t workspace = 0u, workspace_payload;
            if (!p_vm_verify_workspace(largest_bytes, &workspace))
                return image_error(p, "FASL verifier workspace is too large");
            image->verify_bytes = workspace;
            if (!checked_add_size(offsetof(PBytes, data), workspace,
                                  &workspace_payload) ||
                !add_object_budget(workspace_payload, &allocations))
                return image_error(p, "FASL verifier allocation overflow");
        }
        image->required_bytes = allocations;
    }
    return true;
}

static bool parse_top_level(Pocket *p, FASLImage *image)
{
    const FASLSection *section = &image->sections[3];
    size_t cursor = section->offset, end = section->offset + section->length;
    uint32_t count, entry;
    if (!read_u32(image->data, end, &cursor, &count) || count != 1u ||
        !read_u32(image->data, end, &cursor, &entry) || cursor != end ||
        entry >= image->proto_count)
        return image_error(p, "PFAS needs exactly one valid entry proto");
    image->entry_proto = entry;
    return true;
}

static uint32_t field_u32(const uint8_t *data, size_t offset)
{
    return (uint32_t)data[offset] |
           ((uint32_t)data[offset + 1u] << 8u) |
           ((uint32_t)data[offset + 2u] << 16u) |
           ((uint32_t)data[offset + 3u] << 24u);
}

static int64_t signed_i64(uint64_t bits)
{
    if (bits <= (uint64_t)INT64_MAX)
        return (int64_t)bits;
    return -1 - (int64_t)(~bits);
}

static bool validate_references(Pocket *p, FASLImage *image,
                                const FASLSymbol *symbols,
                                const FASLConstant *constants,
                                const FASLProto *protos)
{
    for (uint32_t i = 0u; i < image->constant_count; ++i) {
        const FASLConstant *c = &constants[i];
        size_t cursor = c->payload_offset;
        switch (c->tag) {
        case FASL_CONSTANT_FIXNUM: {
            uint64_t bits = 0u;
            for (unsigned b = 0u; b < 8u; ++b)
                bits |= (uint64_t)image->data[cursor + b] << (8u * b);
            int64_t value = signed_i64(bits);
            if (value < p_fixnum_min() || value > p_fixnum_max())
                return image_error(p, "FASL fixnum is outside runtime range");
            break;
        }
        case FASL_CONSTANT_CHARACTER: {
            uint32_t cp = field_u32(image->data, cursor);
            if (cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu))
                return image_error(p, "invalid FASL Unicode scalar");
            break;
        }
        case FASL_CONSTANT_SYMBOL:
            if (field_u32(image->data, cursor) >= image->symbol_count)
                return image_error(p, "FASL symbol reference is out of range");
            break;
        case FASL_CONSTANT_PROTO:
            if (field_u32(image->data, cursor) >= image->proto_count)
                return image_error(p, "FASL proto reference is out of range");
            break;
        case FASL_CONSTANT_CONS:
            if (field_u32(image->data, cursor) >= image->constant_count ||
                field_u32(image->data, cursor + 4u) >= image->constant_count)
                return image_error(p, "FASL cons reference is out of range");
            break;
        case FASL_CONSTANT_VECTOR: {
            uint32_t length = field_u32(image->data, cursor);
            cursor += 4u;
            for (uint32_t j = 0u; j < length; ++j) {
                if (field_u32(image->data, cursor + (size_t)j * 4u) >=
                    image->constant_count)
                    return image_error(p, "FASL vector reference is out of range");
            }
            break;
        }
        default:
            break;
        }
    }
    for (uint32_t i = 0u; i < image->proto_count; ++i) {
        const FASLProto *proto = &protos[i];
        uint32_t name = proto->field[0], rest = proto->field[2];
        uint32_t code = proto->field[7], pool = proto->field[8];
        if (name >= image->constant_count || code >= image->constant_count ||
            pool >= image->constant_count || rest > 1u ||
            proto->field[6] != p_fixnum_bits())
            return image_error(p, "invalid FASL proto metadata");
        if ((uint64_t)proto->field[1] + rest > proto->field[3])
            return image_error(p, "FASL proto arguments exceed local slots");
        if (constants[name].tag != FASL_CONSTANT_SYMBOL &&
            constants[name].tag != FASL_CONSTANT_STRING)
            return image_error(p, "FASL proto name must be a symbol or string");
        if (constants[code].tag != FASL_CONSTANT_BYTES ||
            constants[pool].tag != FASL_CONSTANT_VECTOR)
            return image_error(p, "FASL proto code or pool has the wrong type");
    }
    if (!parse_top_level(p, image))
        return false;
    {
        const FASLProto *entry = &protos[image->entry_proto];
        if (entry->field[1] != 0u || entry->field[2] != 0u ||
            entry->field[4] != 0u)
            return image_error(p, "PFAS entry proto must take no args or captures");
    }
    (void)symbols;
    return true;
}

bool p_fasl_required_bytes(Pocket *p, Value bytes, size_t *required)
{
    FASLImage image;
    if (required == NULL)
        return image_error(p, "FASL reservation output is null");
    memset(&image, 0, sizeof(image));
    if (!scan_image(p, bytes, &image, NULL, NULL, NULL, true))
        return false;
    /* A safe upper bound for the reusable verifier workspace is based on the
     * largest byte constant. Only proto code constants use it, but the input
     * is not trusted until the proto indices are validated by the loader. */
    *required = image.required_bytes;
    return true;
}

typedef struct {
    FASLSymbol *symbols;
    FASLConstant *constants;
    FASLProto *protos;
    Value *symbol_values;
    Value *constant_values;
    Value *proto_values;
    char *name_buffer;
} FASLScratch;

static bool align_scratch(size_t offset, size_t *aligned)
{
    size_t mask = sizeof(uintptr_t) - 1u;
    if (offset > SIZE_MAX - mask)
        return false;
    *aligned = (offset + mask) & ~mask;
    return true;
}

static bool scratch_layout(FASLImage *image, PBytes *storage,
                           FASLScratch *scratch)
{
    size_t offset = 0u, bytes;
    uint8_t *data = storage->data;
#define TAKE_ARRAY(field, count, type) \
    do { \
        if (!align_scratch(offset, &offset)) return false; \
        scratch->field = (type *)(void *)(data + offset); \
        if (!checked_mul_size((size_t)(count), sizeof(type), &bytes) || \
            !checked_add_size(offset, bytes, &offset)) return false; \
    } while (0)
    TAKE_ARRAY(symbols, image->symbol_count, FASLSymbol);
    TAKE_ARRAY(constants, image->constant_count, FASLConstant);
    TAKE_ARRAY(protos, image->proto_count, FASLProto);
    TAKE_ARRAY(symbol_values, image->symbol_count, Value);
    TAKE_ARRAY(constant_values, image->constant_count, Value);
    TAKE_ARRAY(proto_values, image->proto_count, Value);
    if (!align_scratch(offset, &offset)) return false;
    scratch->name_buffer = (char *)(void *)(data + offset);
#undef TAKE_ARRAY
    return offset <= storage->length;
}

static bool copy_name(Pocket *p, FASLImage *image, size_t offset, uint32_t length,
                      char *buffer, size_t capacity)
{
    if ((size_t)length >= capacity || offset > image->length ||
        (size_t)length > image->length - offset)
        return image_error(p, "FASL package or symbol name exceeds scratch capacity");
    memcpy(buffer, image->data + offset, length);
    buffer[length] = '\0';
    return true;
}

static Value load_symbol(Pocket *p, FASLImage *image, FASLScratch *scratch,
                         uint32_t index)
{
    const FASLSymbol *record = &scratch->symbols[index];
    char *buffer = scratch->name_buffer;
    Value symbol, package;
    if (record->package_length == FASL_SYMBOL_UNINTERNED) {
        if (!copy_name(p, image, record->name_offset, record->name_length,
                       buffer, image->max_name_bytes + 1u))
            return 0u;
        return p_make_symbol(p, buffer);
    }
    if (!copy_name(p, image, record->package_offset, record->package_length,
                   buffer, image->max_name_bytes + 1u))
        return 0u;
    package = p_find_package(p, buffer);
    if (package == 0u)
        package = p_make_package(p, buffer);
    if (package == 0u)
        return 0u;
    if (!copy_name(p, image, record->name_offset, record->name_length,
                   buffer, image->max_name_bytes + 1u))
        return 0u;
    symbol = p_find_symbol(p, package, buffer, NULL);
    if (p->error.code != P_OK)
        return 0u;
    if (symbol != p->nil && p_symbol_ptr(symbol)->home != package) {
        /* The serialized package denotes symbol home identity. INHERITED
         * symbols (notably CL-USER::CAR) must become local shadowing symbols. */
        int status = 0;
        (void)p_find_symbol(p, package, buffer, &status);
        if (p->error.code != P_OK)
            return 0u;
        if (status == 3) {
            if (!p_shadow(p, package, buffer))
                return 0u;
        } else {
            (void)image_error(p, "FASL symbol home package conflicts with an import");
            return 0u;
        }
    }
    symbol = p_intern(p, package, buffer, NULL);
    if (symbol == 0u || p_symbol_ptr(symbol)->home != package) {
        if (p->error.code == P_OK)
            image_error(p, "FASL symbol did not resolve to its declared home package");
        return 0u;
    }
    return symbol;
}

static Value make_symbol_skeleton(Pocket *p, FASLImage *image,
                                  FASLScratch *scratch, uint32_t index)
{
    const FASLSymbol *record = &scratch->symbols[index];
    if (!copy_name(p, image, record->name_offset, record->name_length,
                   scratch->name_buffer, image->max_name_bytes + 1u))
        return 0u;
    return p_make_symbol(p, scratch->name_buffer);
}

static bool make_constant_skeletons(Pocket *p, FASLImage *image,
                                    FASLScratch *scratch)
{
    for (uint32_t i = 0u; i < image->constant_count; ++i) {
        const FASLConstant *constant = &scratch->constants[i];
        const uint8_t *payload = image->data + constant->payload_offset;
        size_t cursor = constant->payload_offset;
        Value value = 0u;
        switch (constant->tag) {
        case FASL_CONSTANT_FIXNUM: {
            uint64_t bits = 0u;
            for (unsigned b = 0u; b < 8u; ++b)
                bits |= (uint64_t)payload[b] << (8u * b);
            value = p_checked_fixnum(p, signed_i64(bits));
            break;
        }
        case FASL_CONSTANT_CHARACTER:
            value = p_character(field_u32(image->data, cursor));
            break;
        case FASL_CONSTANT_FLONUM: {
            uint64_t bits = 0u;
            double number;
            for (unsigned b = 0u; b < 8u; ++b)
                bits |= (uint64_t)payload[b] << (8u * b);
            if (sizeof(number) != 8u || DBL_MANT_DIG != 53 || DBL_MAX_EXP != 1024)
                return image_error(p, "runtime does not support IEEE binary64 FASL floats");
            memcpy(&number, &bits, sizeof(number));
            value = p_flonum(p, number);
            break;
        }
        case FASL_CONSTANT_STRING:
            value = p_string_utf8(p, (const char *)payload,
                                  constant->payload_length);
            break;
        case FASL_CONSTANT_SYMBOL:
            value = scratch->symbol_values[field_u32(image->data, cursor)];
            break;
        case FASL_CONSTANT_CONS:
            value = (Value)p_alloc(p, P_CONS, sizeof(PCons));
            if (value != 0u) {
                p_cons_ptr(value)->car = p->nil;
                p_cons_ptr(value)->cdr = p->nil;
            }
            break;
        case FASL_CONSTANT_VECTOR: {
            uint32_t length = field_u32(image->data, cursor);
            value = p_vector(p, length, p->nil);
            break;
        }
        case FASL_CONSTANT_BYTES:
            value = p_bytes(p, constant->payload_length);
            if (value != 0u && constant->payload_length != 0u)
                memcpy(((PBytes *)value)->data, payload, constant->payload_length);
            break;
        case FASL_CONSTANT_PROTO:
            value = scratch->proto_values[field_u32(image->data, cursor)];
            break;
        default:
            return image_error(p, "unknown FASL constant tag during materialization");
        }
        if (value == 0u)
            return false;
        scratch->constant_values[i] = value;
    }
    return true;
}

static bool fill_constant_graph(Pocket *p, FASLImage *image,
                                FASLScratch *scratch)
{
    for (uint32_t i = 0u; i < image->constant_count; ++i) {
        const FASLConstant *constant = &scratch->constants[i];
        size_t cursor = constant->payload_offset;
        if (constant->tag == FASL_CONSTANT_CONS) {
            PCons *cons = p_cons_ptr(scratch->constant_values[i]);
            cons->car = scratch->constant_values[field_u32(image->data, cursor)];
            cons->cdr = scratch->constant_values[field_u32(image->data, cursor + 4u)];
        } else if (constant->tag == FASL_CONSTANT_VECTOR) {
            PVector *vector = p_vector_ptr(scratch->constant_values[i]);
            uint32_t length = field_u32(image->data, cursor);
            cursor += 4u;
            for (uint32_t j = 0u; j < length; ++j)
                vector->data[j] = scratch->constant_values[
                    field_u32(image->data, cursor + (size_t)j * 4u)];
        }
    }
    (void)p;
    return true;
}

static bool fill_protos(Pocket *p, FASLImage *image, FASLScratch *scratch)
{
    for (uint32_t i = 0u; i < image->proto_count; ++i) {
        const FASLProto *spec = &scratch->protos[i];
        PProto *proto = (PProto *)scratch->proto_values[i];
        proto->name = scratch->constant_values[spec->field[0]];
        proto->required = spec->field[1];
        proto->rest = spec->field[2] != 0u;
        proto->locals = spec->field[3];
        proto->free_count = spec->field[4];
        proto->max_stack = spec->field[5];
        proto->target_bits = spec->field[6];
        proto->code = scratch->constant_values[spec->field[7]];
        proto->constants = scratch->constant_values[spec->field[8]];
        proto->flags = 0u;
    }
    (void)p;
    return true;
}

Value p_fasl_load(Pocket *p, Value bytes)
{
    FASLImage image;
    FASLScratch scratch;
    Value scratch_value, workspace_value, captures, closure;
    PBytes *scratch_storage, *workspace_storage;
    size_t saved;
    memset(&image, 0, sizeof(image));
    /* Parse enough metadata into bounded scratch before any global package
     * mutation. The safe-point caller must already have reserved `required`. */
    if (!scan_image(p, bytes, &image, NULL, NULL, NULL, true))
        return 0u;
    saved = p_root_save(p);
    if (!p_roots(p, &bytes, 1u))
        return 0u;
    scratch_value = p_bytes(p, image.scratch_bytes);
    if (scratch_value == 0u) { p_root_restore(p, saved); return 0u; }
    if (!p_roots(p, &scratch_value, 1u)) { p_root_restore(p, saved); return 0u; }
    scratch_storage = (PBytes *)scratch_value;
    if (!scratch_layout(&image, scratch_storage, &scratch)) {
        p_root_restore(p, saved);
        image_error(p, "FASL scratch layout is invalid");
        return 0u;
    }
    if (!scan_image(p, bytes, &image, scratch.symbols, scratch.constants,
                    scratch.protos, true) ||
        !parse_top_level(p, &image) ||
        !validate_references(p, &image, scratch.symbols, scratch.constants,
                             scratch.protos)) {
        p_root_restore(p, saved);
        return 0u;
    }
    if (image.verify_bytes != 0u) {
        workspace_value = p_bytes(p, image.verify_bytes);
        if (workspace_value == 0u) { p_root_restore(p, saved); return 0u; }
        if (!p_roots(p, &workspace_value, 1u)) { p_root_restore(p, saved); return 0u; }
    } else {
        workspace_value = 0u;
    }
    workspace_storage = workspace_value == 0u ? NULL : (PBytes *)workspace_value;

    /* Build temporary uninterned symbol identities first. This gives the VM
     * verifier real symbol constants while keeping package tables untouched
     * until every proto's code has passed verification. */
    for (uint32_t i = 0u; i < image.symbol_count; ++i) {
        scratch.symbol_values[i] = make_symbol_skeleton(p, &image, &scratch, i);
        if (scratch.symbol_values[i] == 0u) { p_root_restore(p, saved); return 0u; }
    }
    for (uint32_t i = 0u; i < image.proto_count; ++i) {
        scratch.proto_values[i] = (Value)p_alloc(p, P_PROTO, sizeof(PProto));
        if (scratch.proto_values[i] == 0u) { p_root_restore(p, saved); return 0u; }
    }
    if (!make_constant_skeletons(p, &image, &scratch) ||
        !fill_constant_graph(p, &image, &scratch) ||
        !fill_protos(p, &image, &scratch)) {
        p_root_restore(p, saved);
        return 0u;
    }
    if (workspace_storage != NULL) {
        for (uint32_t i = 0u; i < image.proto_count; ++i) {
            if (!p_vm_verify(p, scratch.proto_values[i], workspace_storage->data,
                             workspace_storage->length)) {
                p_root_restore(p, saved);
                return 0u;
            }
            ((PProto *)scratch.proto_values[i])->flags |= P_PROTO_VERIFIED;
        }
    } else {
        (void)image_error(p, "FASL has no verification workspace");
        p_root_restore(p, saved);
        return 0u;
    }
    /* Package mutation starts only after complete image and instruction
     * validation. Replace temporary symbol identities throughout constants
     * and proto metadata, preserving cycles and deterministic symbol IDs. */
    for (uint32_t i = 0u; i < image.symbol_count; ++i) {
        if (scratch.symbols[i].package_length != FASL_SYMBOL_UNINTERNED) {
            scratch.symbol_values[i] = load_symbol(p, &image, &scratch, i);
            if (scratch.symbol_values[i] == 0u) {
                p_root_restore(p, saved);
                return 0u;
            }
        }
    }
    for (uint32_t i = 0u; i < image.constant_count; ++i) {
        const FASLConstant *constant = &scratch.constants[i];
        if (constant->tag == FASL_CONSTANT_SYMBOL) {
            uint32_t symbol_id = field_u32(image.data, constant->payload_offset);
            scratch.constant_values[i] = scratch.symbol_values[symbol_id];
        }
    }
    if (!fill_constant_graph(p, &image, &scratch) ||
        !fill_protos(p, &image, &scratch)) {
        p_root_restore(p, saved);
        return 0u;
    }
    for (uint32_t i = 0u; i < image.proto_count; ++i)
        ((PProto *)scratch.proto_values[i])->flags |= P_PROTO_VERIFIED;
    captures = p_vector(p, 0u, p->nil);
    if (captures == 0u) { p_root_restore(p, saved); return 0u; }
    if (!p_roots(p, &captures, 1u)) { p_root_restore(p, saved); return 0u; }
    closure = (Value)p_alloc(p, P_CLOSURE, sizeof(PClosure));
    if (closure == 0u) { p_root_restore(p, saved); return 0u; }
    {
        PClosure *entry = (PClosure *)closure;
        entry->name = ((PProto *)scratch.proto_values[image.entry_proto])->name;
        entry->parameters = p->nil;
        entry->body = scratch.proto_values[image.entry_proto];
        entry->environment = captures;
        entry->required_count = 0u;
        entry->rest_parameter = 0u;
        entry->flags = P_EVAL_COMPILED;
    }
    p_root_restore(p, saved);
    return closure;
}
