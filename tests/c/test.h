#ifndef POCKET_TEST_H
#define POCKET_TEST_H

#include <stdio.h>
#include <stdlib.h>

/* Tests are ordinary functions: no registration constructors or C extensions. */
typedef void (*TestFunction)(void);
typedef struct {
    const char *name;
    TestFunction run;
} TestCase;

extern unsigned test_assertions;

#define ASSERT(expression) do { \
    ++test_assertions; \
    if (!(expression)) { \
        fprintf(stderr, "%s:%d: assertion failed: %s\n", \
                __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define TEST_CASE(function) { #function, function }

#endif
