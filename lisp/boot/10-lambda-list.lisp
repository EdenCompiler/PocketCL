;;; One shared lambda-list lowering pass for the interpreter and compiler.
;;; The VM only needs required parameters and &REST. Optional/key defaults,
;;; supplied-p flags, validation and macro destructuring become ordinary LET*
;;; bindings. Every function in this file itself uses the restricted subset.

(defun pocket.sys::%body-prefix (body)
  (if (and (stringp (car body)) (cdr body))
      (cons (car body) (pocket.sys::%decl-prefix (cdr body)))
      (pocket.sys::%decl-prefix body)))
(defun pocket.sys::%body-code (body)
  (pocket.sys::%decl-code
   (if (and (stringp (car body)) (cdr body)) (cdr body) body)))
(defun pocket.sys::%body-declarations (body)
  (pocket.sys::%decl-prefix
   (if (and (stringp (car body)) (cdr body)) (cdr body) body)))

(defun pocket.sys::%consume-expression (arguments)
  (let ((temporary (gensym "ARG")))
    (list 'let (list (list temporary (list 'car arguments)))
          (list 'setq arguments (list 'cdr arguments)) temporary)))
(defun pocket.sys::%required-expression (arguments)
  (list 'if arguments (pocket.sys::%consume-expression arguments)
        (list 'pocket.sys::%error "missing required argument")))
(defun pocket.sys::%parameter-name (specification)
  (if (consp specification) (car specification) specification))
(defun pocket.sys::%key-variable (specification)
  (let ((name (pocket.sys::%parameter-name specification)))
    (if (consp name) (cadr name) name)))
(defun pocket.sys::%key-name (specification)
  (let ((name (pocket.sys::%parameter-name specification)))
    (if (consp name) (car name) (intern (symbol-name name) "KEYWORD"))))
(defun pocket.sys::%key-names (parameters active)
  (cond ((null parameters) nil)
        ((eq (car parameters) '&key) (pocket.sys::%key-names (cdr parameters) t))
        ((eq (car parameters) '&aux) nil)
        ((eq (car parameters) '&allow-other-keys)
         (pocket.sys::%key-names (cdr parameters) active))
        (active (cons (pocket.sys::%key-name (car parameters))
                      (pocket.sys::%key-names (cdr parameters) t)))
        (t (pocket.sys::%key-names (cdr parameters) nil))))

(defun pocket.sys::%keyword-pair (arguments keyword)
  (if (null arguments) nil
      (if (eq (car arguments) keyword) arguments
          (pocket.sys::%keyword-pair (cddr arguments) keyword))))
(defun pocket.sys::%keyword-shape (arguments)
  (if (null arguments) t
      (if (and (consp arguments) (consp (cdr arguments)) (symbolp (car arguments)))
          (pocket.sys::%keyword-shape (cddr arguments))
          (pocket.sys::%error "keyword arguments must be symbol/value pairs"))))
(defun pocket.sys::%unknown-keywords (arguments known)
  (if (null arguments) nil
      (if (or (eq (car arguments) :allow-other-keys) (member (car arguments) known))
          (pocket.sys::%unknown-keywords (cddr arguments) known)
          (pocket.sys::%error "unrecognized keyword argument"))))
(defun pocket.sys::%validate-keywords (arguments known allow)
  (pocket.sys::%keyword-shape arguments)
  (if (or allow (let ((pair (pocket.sys::%keyword-pair arguments :allow-other-keys)))
                  (and pair (cadr pair))))
      t (pocket.sys::%unknown-keywords arguments known)))

(defun pocket.sys::%optional-bindings (specification arguments)
  (let ((present (gensym "PRESENT"))
        (name (pocket.sys::%parameter-name specification)))
    (append
     (list (list present (list 'not (list 'null arguments)))
           (list name (list 'if present (pocket.sys::%consume-expression arguments)
                            (if (consp specification) (cadr specification) nil))))
     (if (and (consp specification) (cddr specification))
         (list (list (caddr specification) present)) nil))))
(defun pocket.sys::%key-bindings (specification arguments)
  (let ((pair (gensym "KEY")))
    (append
     (list (list pair (list 'pocket.sys::%keyword-pair arguments
                            (list 'quote (pocket.sys::%key-name specification))))
           (list (pocket.sys::%key-variable specification)
                 (list 'if pair (list 'cadr pair)
                       (if (consp specification) (cadr specification) nil))))
     (if (and (consp specification) (cddr specification))
         (list (list (caddr specification) (list 'not (list 'null pair)))) nil))))

(defun pocket.sys::%ordinary-bindings (parameters arguments mode)
  (if (null parameters) nil
      (let ((specification (car parameters)))
        (cond
          ((eq specification '&optional)
           (pocket.sys::%ordinary-bindings (cdr parameters) arguments :optional))
          ((eq specification '&rest)
           (cons (list (cadr parameters) arguments)
                 (pocket.sys::%ordinary-bindings (cddr parameters) arguments :after-rest)))
          ((eq specification '&key)
           (cons (list (gensym "KEY-CHECK")
                       (list 'pocket.sys::%validate-keywords arguments
                             (list 'quote (pocket.sys::%key-names parameters nil))
                             (if (member '&allow-other-keys parameters) t nil)))
                 (pocket.sys::%ordinary-bindings (cdr parameters) arguments :key)))
          ((eq specification '&allow-other-keys)
           (pocket.sys::%ordinary-bindings (cdr parameters) arguments mode))
          ((eq specification '&aux)
           (pocket.sys::%ordinary-bindings (cdr parameters) arguments :aux))
          ((eq mode :required)
           (cons (list specification (pocket.sys::%required-expression arguments))
                 (pocket.sys::%ordinary-bindings (cdr parameters) arguments mode)))
          ((eq mode :optional)
           (append (pocket.sys::%optional-bindings specification arguments)
                   (pocket.sys::%ordinary-bindings (cdr parameters) arguments mode)))
          ((eq mode :key)
           (append (pocket.sys::%key-bindings specification arguments)
                   (pocket.sys::%ordinary-bindings (cdr parameters) arguments mode)))
          ((eq mode :aux)
           (cons (if (consp specification) specification (list specification nil))
                 (pocket.sys::%ordinary-bindings (cdr parameters) arguments mode)))
          (t (pocket.sys::%error "malformed ordinary lambda list"))))))

(defun pocket.sys::%rich-lambda-p (parameters)
  (if (null parameters) nil
      (if (member (car parameters) '(&optional &key &aux)) t
          (pocket.sys::%rich-lambda-p (cdr parameters)))))
(defun pocket.sys::%accepts-rest-p (parameters)
  (if (null parameters) nil
      (if (atom parameters) t
          (if (member (car parameters) '(&rest &body &key)) t
              (pocket.sys::%accepts-rest-p (cdr parameters))))))
(defun pocket.sys::%extra-argument-check (parameters arguments)
  (if (pocket.sys::%accepts-rest-p parameters) nil
      (list (list 'if arguments (list 'pocket.sys::%error "too many arguments") nil))))
(defun pocket.sys::%expand-lambda (parameters body)
  (if (not (pocket.sys::%rich-lambda-p parameters)) (cons 'lambda (cons parameters body))
      (let ((arguments (gensym "ARGS")))
        (list 'lambda (list '&rest arguments)
              (cons 'let*
                    (cons (pocket.sys::%ordinary-bindings parameters arguments :required)
                          (append (pocket.sys::%body-declarations body)
                                  (pocket.sys::%extra-argument-check parameters arguments)
                                  (pocket.sys::%body-code body))))))))

;;; Macro arguments are syntax, but their binding/default rules are still
;;; expressed as ordinary code. Nested patterns receive separate cursors so
;;; destructuring never mutates the source form or its &WHOLE value.
(defun pocket.sys::%macro-pattern (pattern expression environment)
  (if (null pattern)
      (let ((empty (gensym "EMPTY")))
        (list (list empty expression)
              (list (gensym "EMPTY-CHECK")
                    (list 'if (list 'null empty) nil
                          (list 'pocket.sys::%error "expected an empty destructuring pattern")))))
      (if (symbolp pattern) (list (list pattern expression))
      (let ((whole (gensym "WHOLE")) (arguments (gensym "PATTERN")))
        (append
         (list (list whole expression) (list arguments whole))
         (pocket.sys::%macro-bindings pattern arguments whole environment :required)
         (if (pocket.sys::%accepts-rest-p pattern) nil
             (list (list (gensym "PATTERN-CHECK")
                         (list 'if arguments (list 'pocket.sys::%error "too many destructured arguments") nil)))))))))
(defun pocket.sys::%macro-optional (specification arguments environment)
  (let ((present (gensym "PRESENT")) (value (gensym "VALUE")))
    (append
     (list (list present (list 'not (list 'null arguments)))
           (list value (list 'if present (pocket.sys::%consume-expression arguments)
                             (if (consp specification) (cadr specification) nil))))
     (pocket.sys::%macro-pattern (pocket.sys::%parameter-name specification) value environment)
     (if (and (consp specification) (cddr specification))
         (list (list (caddr specification) present)) nil))))
(defun pocket.sys::%macro-key (specification arguments environment)
  (let ((pair (gensym "KEY")) (value (gensym "VALUE")))
    (append
     (list (list pair (list 'pocket.sys::%keyword-pair arguments
                            (list 'quote (pocket.sys::%key-name specification))))
           (list value (list 'if pair (list 'cadr pair)
                             (if (consp specification) (cadr specification) nil))))
     (pocket.sys::%macro-pattern (pocket.sys::%key-variable specification) value environment)
     (if (and (consp specification) (cddr specification))
         (list (list (caddr specification) (list 'not (list 'null pair)))) nil))))
(defun pocket.sys::%macro-bindings (parameters arguments whole environment mode)
  (cond
    ((null parameters) nil)
    ((symbolp parameters) (list (list parameters arguments)))
    (t
     (let ((specification (car parameters)))
       (cond
         ((eq specification '&whole)
          (cons (list (cadr parameters) whole)
                (pocket.sys::%macro-bindings (cddr parameters) arguments whole environment mode)))
         ((eq specification '&environment)
          (cons (list (cadr parameters) environment)
                (pocket.sys::%macro-bindings (cddr parameters) arguments whole environment mode)))
         ((eq specification '&optional)
          (pocket.sys::%macro-bindings (cdr parameters) arguments whole environment :optional))
         ((or (eq specification '&rest) (eq specification '&body))
          (cons (list (cadr parameters) arguments)
                (pocket.sys::%macro-bindings (cddr parameters) arguments whole environment :after-rest)))
         ((eq specification '&key)
          (cons (list (gensym "KEY-CHECK")
                      (list 'pocket.sys::%validate-keywords arguments
                            (list 'quote (pocket.sys::%key-names parameters nil))
                            (if (member '&allow-other-keys parameters) t nil)))
                (pocket.sys::%macro-bindings (cdr parameters) arguments whole environment :key)))
         ((eq specification '&allow-other-keys)
          (pocket.sys::%macro-bindings (cdr parameters) arguments whole environment mode))
         ((eq specification '&aux)
          (pocket.sys::%macro-bindings (cdr parameters) arguments whole environment :aux))
         ((eq mode :required)
          (append (pocket.sys::%macro-pattern specification
                                             (pocket.sys::%required-expression arguments) environment)
                  (pocket.sys::%macro-bindings (cdr parameters) arguments whole environment mode)))
         ((eq mode :optional)
          (append (pocket.sys::%macro-optional specification arguments environment)
                  (pocket.sys::%macro-bindings (cdr parameters) arguments whole environment mode)))
         ((eq mode :key)
          (append (pocket.sys::%macro-key specification arguments environment)
                  (pocket.sys::%macro-bindings (cdr parameters) arguments whole environment mode)))
         ((eq mode :aux)
          (append (pocket.sys::%macro-pattern (pocket.sys::%parameter-name specification)
                                             (if (consp specification) (cadr specification) nil) environment)
                  (pocket.sys::%macro-bindings (cdr parameters) arguments whole environment mode)))
         (t (pocket.sys::%error "malformed macro lambda list")))))))
(defun pocket.sys::%expand-macro-lambda (parameters body)
  (let ((whole (gensym "FORM")) (environment (gensym "ENV")) (arguments (gensym "ARGS")))
    (list 'lambda (list whole environment)
          (cons 'let*
                (cons (cons (list arguments (list 'cdr whole))
                            (pocket.sys::%macro-bindings parameters arguments whole environment :required))
                      (append (pocket.sys::%body-declarations body)
                              (pocket.sys::%extra-argument-check parameters arguments)
                              (pocket.sys::%body-code body)))))))
(defun pocket.sys::%expand-defmacro (form)
  (list 'pocket.sys::%defmacro (list 'quote (cadr form))
        (list 'function
              (pocket.sys::%expand-macro-lambda
               (caddr form) (pocket.sys::%named-body (cadr form) (cdddr form))))))

(defmacro destructuring-bind (pattern expression &body body)
  (let ((environment (gensym "ENV")))
    (declare (ignore environment))
    (cons 'let* (cons (pocket.sys::%macro-pattern pattern expression nil) body))))

;;; A small portable test front end is available before the full M6 framework.
(defvar pocket.sys::*test-assertions* 0)
(defmacro pocket.sys::%is (expression)
  (list 'progn
        (list 'setq 'pocket.sys::*test-assertions* (list '1+ 'pocket.sys::*test-assertions*))
        (list 'if expression t (list 'pocket.sys::%error "conformance assertion failed"))))
(defmacro pocket.sys::%deftest (name &body body)
  (declare (ignore name))
  (cons 'progn body))
