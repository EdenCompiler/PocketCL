#include "test.h"

#include "eval.h"

#include <string.h>

#ifndef POCKET_TEST_DIR
#define POCKET_TEST_DIR "build/default"
#endif

static const char primitive_test_path[] = POCKET_TEST_DIR "/primitive-test.bin";

static Pocket prim_pocket;
static PState prim_state;

static Value evaluate(const char *source)
{
    PReader reader;
    Value form, result;
    p_clear_error(&prim_pocket);
    p_reader_init(&reader, &prim_pocket, source, strlen(source));
    form = p_read(&reader);
    ASSERT(form != P_EOF);
    if (prim_pocket.error.code != P_OK)
        fprintf(stderr, "primitive test reader failed: %s (%s)\n", source,
                prim_pocket.error.message);
    ASSERT(prim_pocket.error.code == P_OK);
    result = p_eval0(&prim_state, form, prim_pocket.nil);
    if (prim_pocket.error.code != P_OK)
        fprintf(stderr, "primitive test form failed: %s (%s)\n", source,
                prim_pocket.error.message);
    ASSERT(prim_pocket.error.code == P_OK);
    return result;
}

static void expect_fixnum(const char *source, int64_t expected)
{
    Value value = evaluate(source);
    ASSERT(p_is_fixnum(value));
    ASSERT(p_fixnum_value(value) == expected);
}

static void expect_truth(const char *source, bool expected)
{
    Value value = evaluate(source);
    ASSERT(value == (expected ? prim_pocket.truth : prim_pocket.nil));
}

static void expect_error(const char *source, PErrorCode expected)
{
    PReader reader;
    Value form;
    p_clear_error(&prim_pocket);
    p_reader_init(&reader, &prim_pocket, source, strlen(source));
    form = p_read(&reader);
    ASSERT(form != P_EOF);
    ASSERT(prim_pocket.error.code == P_OK);
    (void)p_eval0(&prim_state, form, prim_pocket.nil);
    ASSERT(prim_pocket.error.code == expected);
    p_clear_error(&prim_pocket);
}

void test_prim(void)
{
    Value value, path_symbol, path_value;
    FILE *host_file;

    ASSERT(p_init(&prim_pocket, 1024u * 1024u));
    p_eval_init(&prim_state, &prim_pocket);
    ASSERT(p_install_primitives(&prim_state));
    ASSERT(p_reserve(&prim_pocket, 1024u));
    path_symbol = p_intern(&prim_pocket, prim_pocket.sys,
                           "*PRIMITIVE-TEST-PATH*", NULL);
    ASSERT(path_symbol != 0u);
    path_value = p_string_utf8(&prim_pocket, primitive_test_path,
                               strlen(primitive_test_path));
    ASSERT(path_value != 0u);
    p_symbol_ptr(path_symbol)->value = path_value;
    p_symbol_ptr(path_symbol)->flags |= P_SPECIAL;
    ASSERT(p_symbol_ptr(path_symbol)->value == path_value);
    ASSERT(p_find_symbol(&prim_pocket, prim_pocket.sys,
                         "*PRIMITIVE-TEST-PATH*", NULL) == path_symbol);

    expect_fixnum("(+ 1 2 3)", 6);
    expect_fixnum("(- 10 3 2)", 5);
    expect_fixnum("(* 7 6)", 42);
    expect_fixnum("(/ 84 2 3)", 14);
    expect_truth("(= 12 12.0)", true);
    expect_truth("(/= 1 2 3)", true);
    expect_fixnum("(floor -7 3)", -3);
    ASSERT(prim_state.mv_count == 2u);
    ASSERT(p_is_fixnum(prim_state.mv[1]));
    ASSERT(p_fixnum_value(prim_state.mv[1]) == 2);
    expect_fixnum("(mod -7 3)", 2);
    expect_fixnum("(logand 15 6)", 6);
    expect_fixnum("(ash 3 4)", 48);
    expect_fixnum("(ash 0 1000000000)", 0);
    expect_fixnum("(ash -1 60)", -(INT64_C(1) << 60));
    expect_fixnum("(ash -8 -2)", -2);
    expect_error("(ash 1 64)", P_ERR_RANGE);
    expect_fixnum("(expt 2 10)", 1024);
    expect_fixnum("(expt -1 -3)", -1);
    value = evaluate("(atan 1 1)");
    ASSERT(p_type(value) == P_FLONUM);
    ASSERT(((PFlonum *)value)->number > 0.785398163397 &&
           ((PFlonum *)value)->number < 0.785398163398);
    expect_fixnum("(funcall #'+ 5 6)", 11);
    expect_fixnum("(apply #'+ '(1 2 3 4))", 10);
    expect_error("(/ 7 2)", P_ERR_UNSUPPORTED);
    expect_error("(/ 1 0)", P_ERR_RANGE);
    expect_error("(expt 0 -1)", P_ERR_RANGE);

    expect_truth("(consp (cons 1 2))", true);
    expect_truth("(null nil)", true);
    expect_truth("(simple-vector-p (POCKET.SYS::%MAKE-SIMPLE-VECTOR 1 nil))", true);
    expect_truth("(simple-vector-p \"not-a-vector\")", false);
    expect_truth("(simple-vector-p (POCKET.SYS::%MAKE-BYTES 1))", false);
    value = evaluate("(car (rplacd (cons 9 nil) (cons 8 nil)))");
    ASSERT(p_is_fixnum(value) && p_fixnum_value(value) == 9);

    expect_fixnum("(POCKET.SYS::%SIMPLE-VECTOR-LENGTH (POCKET.SYS::%MAKE-SIMPLE-VECTOR 3 17))", 3);
    expect_fixnum("(POCKET.SYS::%U8-REF (let ((b (POCKET.SYS::%MAKE-BYTES 2))) (POCKET.SYS::%U8-SET b 1 254) b) 1)", 254);
    expect_error("(POCKET.SYS::%U8-SET (POCKET.SYS::%MAKE-BYTES 1) 0 256)", P_ERR_RANGE);
    value = evaluate("(POCKET.SYS::%MAKE-STRING 2 #\\a)");
    ASSERT(p_type(value) == P_STRING && p_string_ptr(value)->length == 2u);
    ASSERT(p_string_ptr(value)->data[0] == (uint32_t)'a');

    value = evaluate("(values 4 5)");
    ASSERT(p_is_fixnum(value) && p_fixnum_value(value) == 4);
    ASSERT(prim_state.mv_count == 2u);
    ASSERT(p_fixnum_value(prim_state.mv[1]) == 5);
    value = evaluate("(funcall #'values 9 10)");
    ASSERT(p_is_fixnum(value) && p_fixnum_value(value) == 9);
    ASSERT(prim_state.mv_count == 2u && p_fixnum_value(prim_state.mv[1]) == 10);
    value = evaluate("(funcall #'values)");
    ASSERT(value == prim_pocket.nil);
    ASSERT(prim_state.mv_count == 0u);

    value = evaluate("(POCKET.SYS::%MAKE-RECORD 'PRIM-RECORD 2 0)");
    ASSERT(p_type(value) == P_RECORD);
    expect_fixnum("(POCKET.SYS::%RECORD-REF (POCKET.SYS::%MAKE-RECORD 'PRIM-RECORD 1 23) 0)", 23);
    value = evaluate("(POCKET.SYS::%MAKE-PACKAGE \"PRIM-TEST-PKG\")");
    ASSERT(p_type(value) == P_PACKAGE);
    expect_truth("(POCKET.SYS::%SET-NICKNAMES \"PRIM-TEST-PKG\" '(\"PTP\"))", true);
    ASSERT(evaluate("(find-package \"PTP\")") == value);
    value = evaluate("(intern \"PRIM-EXPORTED\" \"COMMON-LISP-USER\")");
    ASSERT(p_type(value) == P_SYMBOL);
    expect_truth("(POCKET.SYS::%IMPORT 'COMMON-LISP-USER::PRIM-EXPORTED \"PRIM-TEST-PKG\")", true);
    expect_truth("(POCKET.SYS::%EXPORT 'COMMON-LISP-USER::PRIM-EXPORTED \"PRIM-TEST-PKG\")", true);
    value = evaluate("(POCKET.SYS::%MAKE-PACKAGE \"PRIM-CONSUMER\")");
    ASSERT(p_type(value) == P_PACKAGE);
    expect_truth("(POCKET.SYS::%USE-PACKAGE \"PRIM-TEST-PKG\" \"PRIM-CONSUMER\")", true);
    value = evaluate("(find-symbol \"PRIM-EXPORTED\" \"PRIM-CONSUMER\")");
    ASSERT(value == p_find_symbol(&prim_pocket,
                                  p_find_package(&prim_pocket, "PRIM-TEST-PKG"),
                                  "PRIM-EXPORTED", NULL));
    ASSERT(prim_state.mv_count == 2u);
    ASSERT(prim_state.mv[1] == p_intern(&prim_pocket, prim_pocket.keyword,
                                         "INHERITED", NULL));

    host_file = fopen(primitive_test_path, "wb");
    ASSERT(host_file != NULL);
    ASSERT(fwrite("A\0B", 1u, 3u, host_file) == 3u);
    ASSERT(fclose(host_file) == 0);
    value = evaluate("(POCKET.SYS::%READ-FILE POCKET.SYS::*PRIMITIVE-TEST-PATH*)");
    ASSERT(p_type(value) == P_BYTES && ((PBytes *)value)->length == 3u);
    ASSERT(((PBytes *)value)->data[0] == (uint8_t)'A');
    ASSERT(((PBytes *)value)->data[1] == 0u);
    ASSERT(((PBytes *)value)->data[2] == (uint8_t)'B');
    expect_truth("(POCKET.SYS::%WRITE-FILE POCKET.SYS::*PRIMITIVE-TEST-PATH* (POCKET.SYS::%READ-FILE POCKET.SYS::*PRIMITIVE-TEST-PATH*))", true);
    ASSERT(remove(primitive_test_path) == 0);

    value = evaluate("(intern \"M1-PRIMITIVE-TEST\" \"CL-USER\")");
    ASSERT(p_type(value) == P_SYMBOL);
    ASSERT(p_symbol_ptr(value)->home == prim_pocket.user);
    expect_truth("(boundp 'nil)", true);

    p_eval_destroy(&prim_state);
    p_destroy(&prim_pocket);
}
