#include "pocket.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

static bool roundtrip(Pocket *p, const char *source, bool uninterned)
{
    PReader reader;
    Value roots[2] = {0, 0};
    size_t saved = p_root_save(p);
    char printed[8192], printed_again[8192];
    bool result;

    p_clear_error(p);
    p_reader_init(&reader, p, source, strlen(source));
    roots[0] = p_read(&reader);
    if (roots[0] == 0 || roots[0] == P_EOF || p->error.code != P_OK)
        return false;
    if (!p_roots(p, roots, 2u))
        return false;
    if (!p_debug_print(p, roots[0], printed, sizeof(printed))) {
        p_root_restore(p, saved);
        return false;
    }
    p_reader_init(&reader, p, printed, strlen(printed));
    roots[1] = p_read(&reader);
    if (roots[1] == 0 || roots[1] == P_EOF || p->error.code != P_OK) {
        p_root_restore(p, saved);
        return false;
    }
    if (p_read(&reader) != P_EOF || p->error.code != P_OK) {
        p_root_restore(p, saved);
        return false;
    }
    result = uninterned
        ? p_debug_print(p, roots[1], printed_again, sizeof(printed_again)) &&
          strcmp(printed, printed_again) == 0
        : p_equal(roots[0], roots[1]);
    p_root_restore(p, saved);
    return result;
}

static void expect_reader_error(Pocket *p, const char *source, size_t length,
                                PErrorCode code)
{
    PReader reader;
    Value value;
    p_clear_error(p);
    p_reader_init(&reader, p, source, length);
    value = p_read(&reader);
    ASSERT(value == 0u);
    ASSERT(p->error.code == code);
}

void test_reader(void)
{
    static const char *const corpus[] = {
        "NIL", "T", "CL:NIL", "COMMON-LISP:T", "CL-USER::READER-LOCAL",
        "FOO", "foo", "|Mixed Case|", "FOO\\ BAR", "|A\\|B|", "|lowercase|",
        "|Mañana|", "|λ-symbol|", "#:ODD-NAME", "#:|:UNINTERNED|",
        "()", "(A)", "(A B C)", "(A . B)", "(A B . C)",
        "#()", "#(1 2 3)", "#(NIL T :KEY)", "'A", "''A", "'''A",
        "`(A ,B ,@C)", "`(A `(B ,,C ,@,D))", "#'CAR", "#'|Mixed Fn|",
        "\"hello\"", "\"quote: \\\" and slash: \\\\\"", "\"é and 🌙\"",
        "\"line\\nnot-newline\"", "\"tab\\t-escape-is-t\"", ":KEYWORD",
        "#\\A", "#\\Space", "#\\Newline", "#\\Tab", "#\\Return",
        "#\\x41", "#\\U+1F642", "#\\🌙", "0", "-123", "+123", "123.",
        "#xFF", "#b101010", "#o77", "1.5", "-2.25e+3", ".5", "1d-2",
        "#+POCKET 42", "#+(and pocket (not absent)) 43", "#+(or absent pocket-cl) 44",
        "#-ABSENT 45", "#-POCKET 46 47", "; comment\n 48",
        "#| outer #| nested |# comment |# 49", "(A ; inside list\n B)",
        "COMMON-LISP::NIL", "KEYWORD:POCKET", "POCKET.SYS::%QUASIQUOTE",
        "#(\"vector string\" (nested . tail) #\\x3A)",
        "(1234567890123456789 . -1234567890123456789)",
        "#x0", "#b1", "#o0", "-0", "+0", "1.0", "1e0", "0.0001",
        "(A . (B . C))", "(A (B (C (D))))", "#(())",
        "|.|", "|1E3|", "|A:B|", "FOO\\:BAR", ":|lower keyword|",
        "#\\x20", "#\\U+03BB", "#\\x1F642", "#\\x10FFFF",
        "#+NO-SUCH (discard this form) 50", "#+(and pocket (or pocket-cl no)) 51",
        "#-NO-SUCH 52", "#-POCKET 53 54",
        "#|comment|# 56", "#|one #|two|# three|# 57"
    };
    Pocket pocket;
    size_t i;
    ASSERT(p_init(&pocket, 4u * 1024u * 1024u));
    for (i = 0; i < sizeof(corpus) / sizeof(corpus[0]); ++i) {
        bool uninterned = strstr(corpus[i], "#:") != NULL;
        if (!roundtrip(&pocket, corpus[i], uninterned)) {
            fprintf(stderr, "reader roundtrip failed for: %s (error %d: %s)\n",
                    corpus[i], (int)pocket.error.code, pocket.error.message);
            ASSERT(false);
        }
    }

    /* Parameterized forms push the corpus well past a hundred independent
     * reads while varying numbers, symbols, vectors, syntax prefixes, and
     * feature-condition skip paths. */
    for (i = 0; i < 12u; ++i) {
        char source[192];
        const char *forms[] = {
            "ITEM-%lu", "(|Mixed %lu| ITEM-%lu)", "(ITEM-%lu . %lu)",
            "#(ITEM-%lu :K-%lu %lu)", "'ITEM-%lu", "#'ITEM-%lu",
            "`(ITEM-%lu ,ITEM-%lu ,@'(ITEM-%lu))", "\"utf8-%lu-é🌙\"",
            "#x%lX", "#+POCKET ITEM-%lu", "#-ABSENT OMIT-%lu ITEM-%lu",
            "(+%lu.0e1 %lu)"
        };
        size_t j;
        for (j = 0; j < sizeof(forms) / sizeof(forms[0]); ++j) {
            int written;
            switch (j) {
            case 1: written = snprintf(source, sizeof(source), forms[j],
                                       (unsigned long)i, (unsigned long)i); break;
            case 2: written = snprintf(source, sizeof(source), forms[j],
                                       (unsigned long)i, (unsigned long)(i + 7)); break;
            case 3: written = snprintf(source, sizeof(source), forms[j],
                                       (unsigned long)i, (unsigned long)i,
                                       (unsigned long)(i + 1)); break;
            case 6: written = snprintf(source, sizeof(source), forms[j],
                                       (unsigned long)i, (unsigned long)i,
                                       (unsigned long)i); break;
            case 10: written = snprintf(source, sizeof(source), forms[j],
                                        (unsigned long)i, (unsigned long)(i + 100)); break;
            case 11: written = snprintf(source, sizeof(source), forms[j],
                                        (unsigned long)(i + 1),
                                        (unsigned long)i); break;
            default: written = snprintf(source, sizeof(source), forms[j],
                                        (unsigned long)i); break;
            }
            ASSERT(written > 0 && (size_t)written < sizeof(source));
            ASSERT(roundtrip(&pocket, source, false));
        }
    }

    expect_reader_error(&pocket, ")", 1u, P_ERR_READER);
    expect_reader_error(&pocket, "(. A)", 5u, P_ERR_READER);
    expect_reader_error(&pocket, "(A . B C)", 9u, P_ERR_READER);
    expect_reader_error(&pocket, "(A .)", 5u, P_ERR_READER);
    expect_reader_error(&pocket, "NO-SUCH-PACKAGE::X",
                        strlen("NO-SUCH-PACKAGE::X"), P_ERR_PACKAGE);
    expect_reader_error(&pocket, "COMMON-LISP:NOT-EXPORTED",
                        strlen("COMMON-LISP:NOT-EXPORTED"), P_ERR_PACKAGE);
    expect_reader_error(&pocket, "#.(+ 1 2)", strlen("#.(+ 1 2)"), P_ERR_READER);
    expect_reader_error(&pocket, "#\\xD800", strlen("#\\xD800"), P_ERR_READER);
    expect_reader_error(&pocket, "#\\U+110000", strlen("#\\U+110000"), P_ERR_READER);
    expect_reader_error(&pocket, "#\\Not-A-Character",
                        strlen("#\\Not-A-Character"), P_ERR_READER);
    {
        const char bad_utf8[] = {'"', (char)0xc0, (char)0x80, '"'};
        expect_reader_error(&pocket, bad_utf8, sizeof(bad_utf8), P_ERR_READER);
    }
    {
        const char nul_source[] = {'A', ' ', '\0', 'B'};
        PReader reader;
        p_clear_error(&pocket);
        p_reader_init(&reader, &pocket, nul_source, sizeof(nul_source));
        ASSERT(p_read(&reader) != 0u);
        ASSERT(p_read(&reader) == 0u);
        ASSERT(pocket.error.code == P_ERR_READER);
    }
    {
        char deep[520];
        size_t depth = 257u;
        memset(deep, '(', depth);
        memcpy(deep + depth, "NIL", 3u);
        memset(deep + depth + 3u, ')', depth);
        expect_reader_error(&pocket, deep, depth * 2u + 3u, P_ERR_READER);
    }
    {
        char tiny[1];
        p_clear_error(&pocket);
        ASSERT(!p_debug_print(&pocket, pocket.truth, tiny, sizeof(tiny)));
        ASSERT(pocket.error.code == P_ERR_RANGE);
    }
    p_destroy(&pocket);
}
