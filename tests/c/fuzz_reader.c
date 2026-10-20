/* Deterministic, allocation-bounded reader smoke fuzzer. It is deliberately a
 * standalone executable so CI can run it under address/undefined sanitizers. */
#include "pocket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FUZZ_HEAP_SIZE (256u * 1024u)
#define RESET_EVERY 4096u
#define MAX_SOURCE 128u

static uint64_t random_state = UINT64_C(0x5eed1234c0ffee11);

static uint32_t random32(void)
{
    uint64_t x = random_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    random_state = x;
    return (uint32_t)((x * UINT64_C(2685821657736338717)) >> 32);
}

static void generate_input(unsigned char *bytes, size_t *length)
{
    static const unsigned char grammar[] =
        "()#'``,@.;|\\:\"+-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    static const char *const fragments[] = {
        "(FOO . BAR)", "#(1 2 #\\Space)", "#+POCKET 17",
        "#-ABSENT (X Y)", "#|nested #| block |# comment|#",
        "|escaped \\| name|", "\"text\\\"with\\\\slashes\"", "#xDEADBEEF"
    };
    size_t i;
    *length = random32() % (MAX_SOURCE + 1u);
    if ((random32() & 3u) == 0u) {
        size_t used = 0;
        while (used < *length) {
            const unsigned char *fragment;
            size_t fragment_length;
            const char *selected = fragments[random32() %
                (sizeof(fragments) / sizeof(fragments[0]))];
            fragment = (const unsigned char *)selected;
            fragment_length = strlen(selected);
            for (i = 0; i < fragment_length && used < *length; ++i)
                bytes[used++] = fragment[i];
        }
    } else {
        for (i = 0; i < *length; ++i) {
            uint32_t choice = random32();
            if ((choice & 7u) == 0u)
                bytes[i] = grammar[choice % (sizeof(grammar) - 1u)];
            else
                bytes[i] = (unsigned char)(choice >> 8);
        }
    }
}

static int fuzz_one(Pocket *p, const unsigned char *bytes, size_t length,
                    size_t iteration)
{
    PReader reader;
    size_t forms = 0;
    p_clear_error(p);
    p_reader_init(&reader, p, (const char *)bytes, length);
    for (;;) {
        size_t before = reader.offset;
        Value value = p_read(&reader);
        ++forms;
        if (value == P_EOF)
            return reader.offset <= length ? 0 : 1;
        if (value == 0u)
            return p->error.code == P_OK ? 2 : 0;
        if (p->error.code != P_OK)
            return 3;
        if (reader.offset <= before || reader.offset > length ||
            forms > length + 2u) {
            fprintf(stderr, "reader failed to make bounded progress at input %lu\n",
                    (unsigned long)iteration);
            return 4;
        }
    }
}

int main(int argc, char **argv)
{
    uint64_t iterations = UINT64_C(1000000);
    uint64_t i;
    Pocket pocket;
    unsigned char source[MAX_SOURCE];
    if (argc > 1) {
        char *end = NULL;
        unsigned long long parsed = strtoull(argv[1], &end, 10);
        if (end == argv[1] || *end != '\0') {
            fprintf(stderr, "usage: %s [iteration-count]\n", argv[0]);
            return EXIT_FAILURE;
        }
        iterations = (uint64_t)parsed;
    }
    if (!p_init(&pocket, FUZZ_HEAP_SIZE)) {
        fprintf(stderr, "unable to initialize fuzz runtime: %s\n",
                pocket.error.message);
        return EXIT_FAILURE;
    }
    for (i = 0; i < iterations; ++i) {
        size_t length;
        int result;
        if (i != 0u && i % RESET_EVERY == 0u) {
            p_destroy(&pocket);
            if (!p_init(&pocket, FUZZ_HEAP_SIZE)) {
                fprintf(stderr, "unable to reset fuzz runtime at %lu: %s\n",
                        (unsigned long)i, pocket.error.message);
                return EXIT_FAILURE;
            }
        }
        generate_input(source, &length);
        result = fuzz_one(&pocket, source, length, (size_t)i);
        if (result != 0) {
            fprintf(stderr, "reader fuzz invariant %d failed at input %lu (length %lu)\n",
                    result, (unsigned long)i, (unsigned long)length);
            p_destroy(&pocket);
            return EXIT_FAILURE;
        }
    }
    p_destroy(&pocket);
    printf("Fuzzed %lu deterministic reader inputs.\n", (unsigned long)iterations);
    return EXIT_SUCCESS;
}
