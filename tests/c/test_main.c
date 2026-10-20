#include "test.h"

unsigned test_assertions;

static void foundation_test(void)
{
    /* Replaced by cumulative subsystem tests as each milestone lands. */
    ASSERT(sizeof(unsigned char) == 1);
}

int main(void)
{
    const TestCase cases[] = { TEST_CASE(foundation_test) };
    const size_t count = sizeof(cases) / sizeof(cases[0]);
    size_t i;

    for (i = 0; i < count; ++i) {
        printf("[test] %s\n", cases[i].name);
        cases[i].run();
    }
    printf("Passed %lu tests, %u assertions.\n", (unsigned long)count,
           test_assertions);
    return EXIT_SUCCESS;
}
