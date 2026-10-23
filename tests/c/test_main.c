#include "test.h"

unsigned test_assertions;

void test_memory(void);
void test_package(void);
void test_reader(void);
void test_eval(void);
void test_prim(void);
void test_vm(void);
void test_loader(void);
void test_compiler_prim(void);

static void foundation_test(void)
{
    /* Replaced by cumulative subsystem tests as each milestone lands. */
    ASSERT(sizeof(unsigned char) == 1);
}

int main(void)
{
    const TestCase cases[] = {
        TEST_CASE(foundation_test), TEST_CASE(test_memory), TEST_CASE(test_package),
        TEST_CASE(test_reader), TEST_CASE(test_eval), TEST_CASE(test_prim),
        TEST_CASE(test_vm), TEST_CASE(test_loader), TEST_CASE(test_compiler_prim)
    };
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
