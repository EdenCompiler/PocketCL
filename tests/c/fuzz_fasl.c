#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The fuzz target intentionally links only the C loader/VM. It accepts parser
 * errors, but every successful preflight proceeds through full verification.
 * It never invokes the loaded entry closure. */
typedef struct { uint8_t data[256]; size_t length; } Buffer;

static void byte(Buffer *b, uint8_t value) { b->data[b->length++] = value; }
static void word(Buffer *b, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) byte(b, (uint8_t)(value >> (8u * i)));
}
static size_t section(Buffer *b, uint32_t id)
{
    size_t offset;
    word(b, id); offset = b->length; word(b, 0u); return offset;
}
static void end_section(Buffer *b, size_t offset)
{
    size_t length = b->length - offset - 4u;
    for (unsigned i = 0u; i < 4u; ++i) b->data[offset + i] = (uint8_t)(length >> (8u * i));
}

static Buffer seed_image(unsigned fixnum_bits)
{
    Buffer b = {{0u}, 0u};
    size_t s;
    memcpy(b.data, "PFAS", 4u); b.length = 4u;
    byte(&b, 1u); byte(&b, 0u); byte(&b, 0u);
    byte(&b, fixnum_bits == 31u ? 1u : 0u);
    word(&b, 0u); word(&b, 4u);
    s = section(&b, 1u); word(&b, 1u); word(&b, 7u);
    memcpy(b.data + b.length, "CL-USER", 7u); b.length += 7u;
    word(&b, 3u); memcpy(b.data + b.length, "CAR", 3u); b.length += 3u;
    end_section(&b, s);
    s = section(&b, 2u); word(&b, 5u);
    byte(&b, 5u); word(&b, 4u); word(&b, 0u);
    byte(&b, 7u); word(&b, 16u); word(&b, 3u);
    word(&b, 3u); word(&b, 0u); word(&b, 4u);
    byte(&b, 8u); word(&b, 6u); byte(&b, 1u); word(&b, 0u); byte(&b, 14u);
    byte(&b, 6u); word(&b, 8u); word(&b, 3u); word(&b, 3u);
    byte(&b, 1u); word(&b, 8u);
    for (unsigned i = 0u; i < 8u; ++i)
        byte(&b, (uint8_t)((uint64_t)42u >> (8u * i)));
    end_section(&b, s);
    s = section(&b, 3u); word(&b, 1u);
    word(&b, 0u); word(&b, 0u); word(&b, 0u); word(&b, 0u); word(&b, 0u);
    word(&b, 1u); word(&b, fixnum_bits); word(&b, 2u); word(&b, 1u);
    end_section(&b, s);
    s = section(&b, 4u); word(&b, 1u); word(&b, 0u); end_section(&b, s);
    for (unsigned i = 0u; i < 4u; ++i) b.data[8u + i] = (uint8_t)(b.length >> (8u * i));
    return b;
}

static uint32_t random_word(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13u; x ^= x >> 17u; x ^= x << 5u;
    *state = x;
    return x;
}

int main(int argc, char **argv)
{
    unsigned long iterations = 1000000ul;
    uint32_t random_state = 0x70666173u;
    Buffer seed;
    Pocket p;
    if (argc > 1) iterations = strtoul(argv[1], NULL, 10);
    if (!p_init(&p, 2u * 1024u * 1024u)) return EXIT_FAILURE;
    seed = seed_image(p_fixnum_bits());

    for (unsigned long n = 0u; n < iterations; ++n) {
        uint8_t input[256];
        size_t length;
        Value bytes;
        size_t saved, required = 0u;
        bool shaped = (n % 4u) == 0u;
        if ((n % 512u) == 511u) {
            p_destroy(&p);
            if (!p_init(&p, 2u * 1024u * 1024u)) return EXIT_FAILURE;
        }
        if (shaped) {
            length = seed.length;
            memcpy(input, seed.data, length);
            if ((n % 16u) != 0u) {
                size_t changes = 1u + random_word(&random_state) % 5u;
                while (changes-- != 0u) {
                    size_t at = random_word(&random_state) % length;
                    input[at] ^= (uint8_t)(1u + random_word(&random_state) % 255u);
                }
            }
        } else {
            length = random_word(&random_state) % 129u;
            for (size_t i = 0u; i < length; ++i)
                input[i] = (uint8_t)random_word(&random_state);
        }
        bytes = p_bytes(&p, length);
        if (bytes == 0u) {
            p_destroy(&p);
            if (!p_init(&p, 2u * 1024u * 1024u)) return EXIT_FAILURE;
            continue;
        }
        if (length != 0u) memcpy(((PBytes *)bytes)->data, input, length);
        saved = p_root_save(&p);
        if (!p_roots(&p, &bytes, 1u)) return EXIT_FAILURE;
        p_clear_error(&p);
        if (p_fasl_required_bytes(&p, bytes, &required)) {
            if (p_reserve(&p, required)) {
                (void)p_fasl_load(&p, bytes);
            }
        }
        p_root_restore(&p, saved);
        p_clear_error(&p);
    }
    p_destroy(&p);
    printf("PFAS fuzz completed %lu inputs\n", iterations);
    return EXIT_SUCCESS;
}
