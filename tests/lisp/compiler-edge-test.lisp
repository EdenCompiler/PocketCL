(in-package :pocket.test)

;;; Compiler edge cases whose source identity or binding kind changes across
;;; nested scopes. Kept apart from the smoke tests so each regression states
;;; one specific compiler contract.
(pocket.sys::%proclaim-special '*compiler-edge-dynamic*)
(setq *compiler-edge-dynamic* :root)

(deftest compiler-edge-captured-special-declaration
  ;; The reader's free SPECIAL declaration sees the caller's dynamic LET
  ;; binding, while the outer lexical binding remains visible after the LET.
  (let ((edge-value :lexical))
    (let ((reader
            (lambda ()
              (declare (special edge-value))
              edge-value)))
      (is (eq :dynamic
              (let ((edge-value :dynamic))
                (declare (special edge-value))
                (funcall reader))))
      (is (eq :lexical edge-value)))))

(deftest compiler-edge-inner-let-shadows-free-special
  ;; An enclosing free SPECIAL declaration changes references, but does not
  ;; make an inner LET binding special. The inner binding must shadow it.
  (is (eq :inner-lexical
          (let ((edge-value :outer-lexical))
            (locally (declare (special edge-value))
              (let ((edge-value :inner-lexical))
                edge-value)))))
  (is (eq :root *compiler-edge-dynamic*)))

(deftest compiler-edge-reused-source-cons
  ;; The exact same source cons occurs under three analysis contexts. Its
  ;; symbol-macro expansion must follow the context at each occurrence.
  (let* ((shared '(progn alias))
         (form (list 'let (list (list 'value 5))
                     (list 'symbol-macrolet
                           (list (list 'alias 'value))
                           (list 'list shared
                                 (list 'let (list (list 'value 9)) shared)
                                 shared))))
         (proto (pocket-compiler::compile-form form)))
    (is-equal '(5 9 5)
              (funcall (pocket.sys::%make-closure proto (vector)))))
  ;; LET analysis data also depends on the lexical context. Reusing a source
  ;; LET under an inner VALUE binding must retain both frames' slot layouts.
  (let* ((shared '(let ((copy value)) copy))
         (form (list 'let (list (list 'value 5))
                     (list 'list shared
                           (list 'let (list (list 'value 9)) shared)
                           shared)))
         (proto (pocket-compiler::compile-form form)))
    (is-equal '(5 9 5)
              (funcall (pocket.sys::%make-closure proto (vector))))))

(deftest compiler-edge-local-function-shadows-hot-operator
  ;; Local FUNCTION bindings must take precedence over the compiler's direct
  ;; arithmetic opcode selection, including calls through FUNCTION.
  (is (= 99 (flet ((+ (left right)
                     (declare (ignore left right))
                     99))
              (+ 1 2))))
  (is (= 77 (flet ((car (value)
                     (declare (ignore value))
                     77))
              (funcall #'car '(1 2))))))

(deftest compiler-edge-values-and-unwind-order
  (is (= 13 (+ 10 (multiple-value-prog1 (values 3 4) (values 8 9)))))
  (is-equal '(3 4)
            (multiple-value-call #'list
              (multiple-value-prog1 (values 3 4) (values 8 9))))
  (setq *compiler-edge-dynamic* :root)
  (is (eq :inner
          (catch 'compiler-edge-tag
            (let ((*compiler-edge-dynamic* :inner))
              (unwind-protect
                   (throw 'compiler-edge-tag *compiler-edge-dynamic*)
                (setq *compiler-edge-dynamic* :cleanup))))))
  (is (eq :root *compiler-edge-dynamic*)))

(finish)
