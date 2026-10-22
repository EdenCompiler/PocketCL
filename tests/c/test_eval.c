#include "eval.h"
#include "test.h"

#include <string.h>

static Value read_form(Pocket *p, const char *source)
{
    PReader reader;
    p_reader_init(&reader, p, source, strlen(source));
    return p_read(&reader);
}

static Value evaluate(PState *state, const char *source)
{
    Value form = read_form(state->p, source);
    return p_eval0(state, form, 0u);
}

void test_eval(void)
{
    Pocket p;
    PState state;
    ASSERT(p_init(&p, 4u * 1024u * 1024u));
    p_eval_init(&state, &p);
    ASSERT(state.initialized);
    ASSERT(p_install_primitives(&state));

    ASSERT(p_is_fixnum(evaluate(&state, "(if t 11 12)")));
    ASSERT(p_fixnum_value(evaluate(&state, "(if nil 11 12)")) == 12);
    ASSERT(p_fixnum_value(evaluate(&state,
        "(let ((x 3)) (let ((f (function (lambda (y) (+ x y))))) (funcall f 4)))")) == 7);

    /* The control operators exercise both lexical lookup and dynamic unwind. */
    ASSERT(p_fixnum_value(evaluate(&state, "(block done (return-from done 37) 0)")) == 37);
    ASSERT(p_fixnum_value(evaluate(&state, "(catch 'tag (throw 'tag 41))")) == 41);
    ASSERT(evaluate(&state, "(tagbody start (go finish) 0 finish)") == p.nil);

    /* Tail recursion must reuse the apply loop rather than the C stack. */
    ASSERT(p_fixnum_value(evaluate(&state,
        "(labels ((loop (n) (if (= n 0) 17 (loop (- n 1))))) (loop 1000))")) == 17);
    ASSERT(p_fixnum_value(evaluate(&state,
        "(labels ((loop (n) (if (= n 0) 17 "
        "(funcall (function loop) (- n 1))))) (loop 1000))")) == 17);
    ASSERT(p_fixnum_value(evaluate(&state,
        "(labels ((loop (n) (if (= n 0) 17 "
        "(apply (function loop) (cons (- n 1) nil))))) (loop 1000))")) == 17);

    /* These names remain ordinary lexical function names when shadowed. */
    ASSERT(p_fixnum_value(evaluate(&state,
        "(flet ((funcall (fn &rest args) 91)) (funcall #'+ 1 2))")) == 91);
    ASSERT(p_fixnum_value(evaluate(&state,
        "(flet ((apply (fn &rest args) 92)) (apply #'+ 1 '(2)))")) == 92);

    /* The first-class VALUES primitive must preserve zero returned values. */
    ASSERT(evaluate(&state, "(funcall #'values)") == p.nil);
    ASSERT(state.mv_count == 0u);

    p_eval_destroy(&state);
    p_destroy(&p);
}
