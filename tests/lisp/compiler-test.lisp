(in-package :pocket.test)

;;; These checks deliberately run through the VM compiler's emitted code. They
;;; cover binding identity and control flow that simple arithmetic smoke tests
;;; cannot exercise.
(defvar *compiler-eval-when-count* 0)
(defvar *compiler-nested-eval-when-count* 0)
(defvar *compiler-cleanup-count* 0)
(defvar *compiler-dynamic-value* 10)
(setq compiler-scope-value 1)
(pocket.sys::%proclaim-special '*compiler-dynamic-value*)

(defun compiler-rejects-form-p (form)
  (handler-case
      (progn (pocket-compiler::compile-form form) nil)
    (error () t)))

(deftest compiler-captured-assignment-is-shared
  (is (= 9
         (let ((value 3))
           (let ((reader (lambda () value)))
             (setq value 9)
             (funcall reader))))))

(deftest compiler-grandparent-capture-is-relayed
  (is (= 17
         (let ((value 17))
           (funcall (funcall (lambda () (lambda () value))))))))

(deftest compiler-parallel-and-sequential-let-bindings
  (is-equal '(1 2)
            (let ((value 1))
              (let ((value 2) (outer value))
                (list outer value))))
  (is-equal '(2 2)
            (let ((value 1))
              (let* ((value 2) (copy value))
                (list value copy)))))

(deftest compiler-multiple-value-call-harvests-left-to-right
  (is-equal '(1 2 3 4)
            (multiple-value-call #'list (values 1 2) (values 3 4))))

(deftest compiler-multiple-value-prog1-restores-values
  (is-equal '(1 2)
            (multiple-value-list
             (multiple-value-prog1 (values 1 2) (values 8 9)))))

(deftest compiler-local-functions-use-function-namespace
  (is (= 8 (flet ((double (x) (+ x x))) (double 4))))
  (is (= 120
         (labels ((fact (n)
                    (if (= n 0) 1 (* n (fact (1- n))))))
           (fact 5)))))

(deftest compiler-function-lambda-normalizes-rich-parameters
  (is (= 10 (funcall #'(lambda (&optional (value 10)) value))))
  (is (= 12 (funcall #'(lambda (&optional (value 10)) value) 12))))

(deftest compiler-rejects-constant-variable-bindings
  (is (compiler-rejects-form-p '(function (lambda (nil) nil))))
  (is (compiler-rejects-form-p '(let ((t nil)) t)))
  (is (compiler-rejects-form-p '(symbol-macrolet ((:bad 1)) :bad))))

(deftest compiler-literal-arithmetic-folds-to-verified-constant
  (let* ((proto (pocket-compiler::compile-form '(+ 20 22)))
         (constants (pocket.sys::%proto-ref proto 8)))
    ;; The result is in the constant pool, while the runtime guard remains in
    ;; bytecode to preserve function-cell redefinition semantics.
    (is (= 42 (svref constants 2)))))

(deftest compiler-literal-folding-honors-function-redefinition
  (let* ((proto (pocket-compiler::compile-form '(+ 10 20)))
         (original-plus (symbol-function '+))
         (result nil))
    (unwind-protect
         (progn
           (pocket.sys::%set-symbol-function
            '+ (lambda (left right)
                 (declare (ignore left right))
                 87))
           (setq result (funcall (pocket.sys::%make-closure proto (vector)))))
      (pocket.sys::%set-symbol-function '+ original-plus))
    (is (= result 87))))

(deftest compiler-literal-folding-checks-target-boundaries
  (let* ((minimum (ash -1 (1- (pocket.sys::%fixnum-bits))))
         (maximum (- -1 minimum)))
    (multiple-value-bind (value foldable)
        (pocket-compiler::cg-folded-hot-value '+ maximum 1)
      (declare (ignore value))
      (is (not foldable)))
    (multiple-value-bind (value foldable)
        (pocket-compiler::cg-folded-hot-value '- minimum 1)
      (declare (ignore value))
      (is (not foldable)))
    (multiple-value-bind (value foldable)
        (pocket-compiler::cg-folded-hot-value '+ maximum -1)
      (is foldable)
      (is (= value (1- maximum))))))

(deftest compiler-special-bindings-are-dynamic
  (is (= 22
         (let ((*compiler-dynamic-value* 22))
           *compiler-dynamic-value*)))
  (is (= 10 *compiler-dynamic-value*)))

(deftest compiler-locally-special-declaration-stays-in-scope
  (let ((compiler-scope-value 2))
    (is (= 2 compiler-scope-value))
    (is (= 1 (locally (declare (special compiler-scope-value))
              compiler-scope-value)))
    (is (= 2 compiler-scope-value))))

(deftest compiler-block-catch-and-tagbody-transfers
  (is (= 6 (block done (return-from done (+ 2 4)) 99)))
  (is (= 7 (catch 'compiler-tag (throw 'compiler-tag 7))))
  (is-equal 3 (let ((sum 0))
             (tagbody
               (setq sum 1)
               next
               (setq sum (+ sum 2))
               (go finished)
               (setq sum 99)
              finished)
             sum)))

(deftest compiler-unwind-protect-runs-once
  (setq *compiler-cleanup-count* 0)
  (is (= 5
         (unwind-protect 5
           (setq *compiler-cleanup-count* (1+ *compiler-cleanup-count*)))))
  (is (= 1 *compiler-cleanup-count*)))

(deftest compiler-local-macro-and-symbol-macro-scopes
  (is (= 7
         (macrolet ((constant-seven () 7))
           (constant-seven))))
  (is (= 12
         (let ((value 12))
           (symbol-macrolet ((alias value)) alias))))
  ;; A nearer ordinary variable prevents an outer symbol macro from leaking.
  (is-equal '(4 9)
            (let ((value 4))
              (symbol-macrolet ((alias value))
                (let ((alias 9)) (list value alias))))))

(deftest compiler-symbol-macro-expansions-follow-occurrence-context
  ;; The same interned symbol occurs in each lexical contour. Its expansion
  ;; must come from the nearest active symbol-macro binding at that occurrence.
  (is-equal '(5 7 9 5)
            (let ((value 5))
              (symbol-macrolet ((alias value))
                (list alias
                      (symbol-macrolet ((alias 7)) alias)
                      (let ((alias 9)) alias)
                      alias))))
  ;; NIL is a valid expansion, distinct from no symbol-macro binding.
  (is (= 2 (symbol-macrolet ((nothing nil))
             (if nothing 1 2)))))

(deftest compiler-eval-when-compiles-and-executes-once
  (setq *compiler-eval-when-count* 0)
  (multiple-value-bind (proto result)
      (pocket-compiler::process-toplevel
       '(eval-when (:compile-toplevel :load-toplevel)
          (setq *compiler-eval-when-count* (1+ *compiler-eval-when-count*))))
    (is proto)
    (is (= result 1)))
  (is (= *compiler-eval-when-count* 1)))

(deftest compiler-nested-eval-when-executes-only-execute
  (setq *compiler-nested-eval-when-count* 0)
  (let ((proto
          (pocket-compiler::compile-form
           '(progn
              (eval-when (:load-toplevel)
                (setq *compiler-nested-eval-when-count* 100))
              (eval-when (:execute)
                (setq *compiler-nested-eval-when-count* 1))))))
    (funcall (pocket.sys::%make-closure proto (vector))))
  (is (= *compiler-nested-eval-when-count* 1)))

(finish)
