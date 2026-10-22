;;; Stage-0 boot layer. Only core operators, required/&rest parameters, and
;;; primitive calls appear before their Lisp definitions below. Internal
;;; helpers stay in POCKET.SYS; standard definitions live in COMMON-LISP.
;;; The host initially reads this file with *PACKAGE* = COMMON-LISP.

(pocket.sys::%defun 'list #'(lambda (&rest items) items))
(pocket.sys::%defun 'identity #'(lambda (value) value))
(pocket.sys::%defun 'caar #'(lambda (x) (car (car x))))
(pocket.sys::%defun 'cadr #'(lambda (x) (car (cdr x))))
(pocket.sys::%defun 'cdar #'(lambda (x) (cdr (car x))))
(pocket.sys::%defun 'cadar #'(lambda (x) (car (cdr (car x)))))
(pocket.sys::%defun 'cddr #'(lambda (x) (cdr (cdr x))))
(pocket.sys::%defun 'caadr #'(lambda (x) (car (car (cdr x)))))
(pocket.sys::%defun 'caddr #'(lambda (x) (car (cdr (cdr x)))))
(pocket.sys::%defun 'cdddr #'(lambda (x) (cdr (cdr (cdr x)))))
(pocket.sys::%defun 'cadddr #'(lambda (x) (car (cdr (cdr (cdr x))))))
(pocket.sys::%defun 'first #'(lambda (x) (car x)))
(pocket.sys::%defun 'second #'(lambda (x) (cadr x)))
(pocket.sys::%defun 'third #'(lambda (x) (caddr x)))
(pocket.sys::%defun 'rest #'(lambda (x) (cdr x)))
(pocket.sys::%defun '1+ #'(lambda (x) (+ x 1)))
(pocket.sys::%defun '1- #'(lambda (x) (- x 1)))

(pocket.sys::%defun 'pocket.sys::%append-two
  #'(lambda (left right)
      (if (null left) right
          (cons (car left) (pocket.sys::%append-two (cdr left) right)))))
(pocket.sys::%defun 'append
  #'(lambda (&rest lists)
      (if (null lists) nil
          (if (null (cdr lists)) (car lists)
              (pocket.sys::%append-two (car lists) (apply #'append (cdr lists)))))))
(pocket.sys::%defun 'list*
  #'(lambda (first &rest rest)
      (if (null rest) first (cons first (apply #'list* rest)))))
(pocket.sys::%defun 'pocket.sys::%reverse-list
  #'(lambda (items result)
      (if (null items) result
          (pocket.sys::%reverse-list (cdr items) (cons (car items) result)))))
(pocket.sys::%defun 'reverse
  #'(lambda (items) (pocket.sys::%reverse-list items nil)))
(pocket.sys::%defun 'pocket.sys::%nreverse-list
  #'(lambda (items result)
      (if (null items) result
          (let ((next (cdr items)))
            (rplacd items result)
            (pocket.sys::%nreverse-list next items)))))
(pocket.sys::%defun 'nreverse
  #'(lambda (items) (pocket.sys::%nreverse-list items nil)))
(pocket.sys::%defun 'copy-list
  #'(lambda (items)
      (if (consp items) (cons (car items) (copy-list (cdr items))) items)))
(pocket.sys::%defun 'pocket.sys::%list-length
  #'(lambda (items count)
      (if (null items) count (pocket.sys::%list-length (cdr items) (1+ count)))))
(pocket.sys::%defun 'length
  #'(lambda (sequence)
      (if (listp sequence) (pocket.sys::%list-length sequence 0)
          (if (stringp sequence) (pocket.sys::%string-length sequence)
              (pocket.sys::%simple-vector-length sequence)))))
(pocket.sys::%defun 'nthcdr
  #'(lambda (n items)
      (if (< n 0) (pocket.sys::%error "NTHCDR requires a nonnegative index")
          (if (= n 0) items (nthcdr (1- n) (cdr items))))))
(pocket.sys::%defun 'nth #'(lambda (n items) (car (nthcdr n items))))
(pocket.sys::%defun 'member
  #'(lambda (item items &rest options)
      (if options (pocket.sys::%error "MEMBER options arrive with the data library")
          (if (null items) nil
              (if (eql item (car items)) items (member item (cdr items)))))))
(pocket.sys::%defun 'assoc
  #'(lambda (item entries &rest options)
      (if options (pocket.sys::%error "ASSOC options arrive with the data library")
          (if (null entries) nil
              (if (consp (car entries))
                  (if (eql item (caar entries)) (car entries) (assoc item (cdr entries)))
                  (assoc item (cdr entries)))))))
(pocket.sys::%defun 'pocket.sys::%any-null
  #'(lambda (lists)
      (if (null lists) nil
          (if (null (car lists)) t (pocket.sys::%any-null (cdr lists))))))
(pocket.sys::%defun 'pocket.sys::%cars
  #'(lambda (lists)
      (if (null lists) nil (cons (caar lists) (pocket.sys::%cars (cdr lists))))))
(pocket.sys::%defun 'pocket.sys::%cdrs
  #'(lambda (lists)
      (if (null lists) nil (cons (cdar lists) (pocket.sys::%cdrs (cdr lists))))))
(pocket.sys::%defun 'mapcar
  #'(lambda (function first &rest others)
      (let ((lists (cons first others)))
        (if (pocket.sys::%any-null lists) nil
            (cons (apply function (pocket.sys::%cars lists))
                  (apply #'mapcar function (pocket.sys::%cdrs lists)))))))
(pocket.sys::%defun 'complement
  #'(lambda (function) #'(lambda (&rest args) (not (apply function args)))))
(pocket.sys::%defun 'constantly
  #'(lambda (value) #'(lambda (&rest ignored) (declare (ignore ignored)) value)))

;;; Build forms explicitly until the backquote expander is installed.
(pocket.sys::%defun 'pocket.sys::%decl-prefix
  #'(lambda (body)
      (if (consp (car body))
          (if (eq (caar body) 'declare)
              (cons (car body) (pocket.sys::%decl-prefix (cdr body))) nil) nil)))
(pocket.sys::%defun 'pocket.sys::%decl-code
  #'(lambda (body)
      (if (consp (car body))
          (if (eq (caar body) 'declare)
              (pocket.sys::%decl-code (cdr body)) body) body)))
(pocket.sys::%defun 'pocket.sys::%body-prefix
  #'(lambda (body)
      (if (stringp (car body))
          (if (cdr body) (cons (car body) (pocket.sys::%decl-prefix (cdr body))) nil)
          (pocket.sys::%decl-prefix body))))
(pocket.sys::%defun 'pocket.sys::%body-code
  #'(lambda (body)
      (if (stringp (car body))
          (if (cdr body) (pocket.sys::%decl-code (cdr body)) body)
          (pocket.sys::%decl-code body))))
;;; Function and macro definitions share the implicit named block rule. Keep
;;; declarations outside the block so their scope includes parameter bindings.
(pocket.sys::%defun 'pocket.sys::%named-body
  #'(lambda (name body)
      (append (pocket.sys::%body-prefix body)
              (list (cons 'block (cons name (pocket.sys::%body-code body)))))))
(pocket.sys::%defmacro 'defun
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'pocket.sys::%defun (list 'quote (cadr form))
            (list 'function
                  (cons 'lambda
                        (cons (caddr form)
                              (pocket.sys::%named-body (cadr form) (cdddr form))))))))
(pocket.sys::%defmacro 'lambda
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'function form)))
(pocket.sys::%defmacro 'defmacro
  #'(lambda (form environment)
      (declare (ignore environment))
      (pocket.sys::%expand-defmacro form)))
(pocket.sys::%defmacro 'when
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'if (cadr form) (cons 'progn (cddr form)) nil)))
(pocket.sys::%defmacro 'unless
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'if (cadr form) nil (cons 'progn (cddr form)))))
(pocket.sys::%defmacro 'and
  #'(lambda (form environment)
      (declare (ignore environment))
      (if (null (cdr form)) t
          (if (null (cddr form)) (cadr form)
              (list 'if (cadr form) (cons 'and (cddr form)) nil)))))

(defun string (designator)
  (if (stringp designator) designator
      (if (symbolp designator) (symbol-name designator)
          (if (characterp designator) (pocket.sys::%make-string 1 designator)
              (pocket.sys::%error "not a string designator")))))
(defun pocket.sys::%copy-string-part (target start source index)
  (if (= index (pocket.sys::%string-length source)) target
      (progn
        (pocket.sys::%set-schar target (+ start index) (schar source index))
        (pocket.sys::%copy-string-part target start source (1+ index)))))
(defun pocket.sys::%join-strings (left right)
  (let ((output (pocket.sys::%make-string (+ (length left) (length right)) #\Space)))
    (pocket.sys::%copy-string-part output 0 left 0)
    (pocket.sys::%copy-string-part output (length left) right 0)))
(defun pocket.sys::%integer-digits (n)
  (if (< n 10) (list (code-char (+ 48 n)))
      (append (pocket.sys::%integer-digits (truncate n 10))
              (list (code-char (+ 48 (rem n 10)))))))
(defun pocket.sys::%fill-string (output chars index)
  (if (null chars) output
      (progn (pocket.sys::%set-schar output index (car chars))
             (pocket.sys::%fill-string output (cdr chars) (1+ index)))))
(defun pocket.sys::%integer-string (n)
  (if (< n 0)
      (pocket.sys::%join-strings "-" (pocket.sys::%integer-string (- n)))
      (let ((digits (pocket.sys::%integer-digits n)))
        (pocket.sys::%fill-string (pocket.sys::%make-string (length digits) #\Space) digits 0))))
(pocket.sys::%proclaim-special '*gensym-counter*)
(set '*gensym-counter* 0)
(export '*gensym-counter* 'cl)
(defun gensym (&rest options)
  (let* ((option (if options (car options) "G"))
         (number (if (integerp option) option *gensym-counter*))
         (prefix (if (stringp option) option "G")))
    (if (< number 0) (pocket.sys::%error "GENSYM counter must be nonnegative") nil)
    (if (integerp option) nil (setq *gensym-counter* (1+ *gensym-counter*)))
    (make-symbol (pocket.sys::%join-strings prefix (pocket.sys::%integer-string number)))))

(pocket.sys::%defmacro 'or
  #'(lambda (form environment)
      (declare (ignore environment))
      (if (null (cdr form)) nil
          (if (null (cddr form)) (cadr form)
              (let ((temporary (gensym "OR")))
                (list 'let (list (list temporary (cadr form)))
                      (list 'if temporary temporary (cons 'or (cddr form)))))))))
(pocket.sys::%defmacro 'cond
  #'(lambda (form environment)
      (declare (ignore environment))
      (if (null (cdr form)) nil
          (let ((clause (cadr form)))
            (if (null (cdr clause))
                (list 'or (car clause) (cons 'cond (cddr form)))
                (list 'if (car clause) (cons 'progn (cdr clause))
                      (cons 'cond (cddr form))))))))
(pocket.sys::%defmacro 'defvar
  #'(lambda (form environment)
      (declare (ignore environment))
      (let ((name (list 'quote (cadr form))))
        (list 'progn (list 'pocket.sys::%proclaim-special name)
              (if (cddr form)
                  (list 'if (list 'boundp name) nil (list 'set name (caddr form))) nil)
              name))))
(pocket.sys::%defmacro 'defparameter
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'progn (list 'pocket.sys::%proclaim-special (list 'quote (cadr form)))
            (list 'set (list 'quote (cadr form)) (caddr form))
            (list 'quote (cadr form)))))
(pocket.sys::%defmacro 'defconstant
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'pocket.sys::%define-constant (list 'quote (cadr form)) (caddr form))))

;;; MV-BIND lowers to the core multiple-value call. Missing values are NIL
;;; because CAR/CDR of NIL are NIL; the generated lambda accepts all extras.
(defun pocket.sys::%mv-bindings (names values)
  (if (null names) nil
      (cons (list (car names) (list 'car values))
            (pocket.sys::%mv-bindings (cdr names) (list 'cdr values)))))
(pocket.sys::%defmacro 'multiple-value-bind
  #'(lambda (form environment)
      (declare (ignore environment))
      (let ((temporary (gensym "VALUES")))
        (list 'multiple-value-call
              (list 'function
                    (list 'lambda (list '&rest temporary)
                          (cons 'let (cons (pocket.sys::%mv-bindings (cadr form) temporary)
                                           (cdddr form)))))
              (caddr form)))))
(pocket.sys::%defmacro 'multiple-value-list
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'multiple-value-call (list 'function 'list) (cadr form))))
(pocket.sys::%defmacro 'nth-value
  #'(lambda (form environment)
      (declare (ignore environment))
      (let ((index (gensym "INDEX")))
        (list 'let (list (list index (cadr form)))
              (list 'nth index (list 'multiple-value-list (caddr form)))))))
(defun values-list (items) (apply #'values items))

(defun pocket.sys::%vector-list (vector index)
  (if (= index (pocket.sys::%simple-vector-length vector)) nil
      (cons (svref vector index) (pocket.sys::%vector-list vector (1+ index)))))
(defun pocket.sys::%fill-vector (vector items index)
  (if (null items) vector
      (progn (pocket.sys::%set-svref vector index (car items))
             (pocket.sys::%fill-vector vector (cdr items) (1+ index)))))
(defun vector (&rest items)
  (pocket.sys::%fill-vector (pocket.sys::%make-simple-vector (length items) nil) items 0))

;;; Backquote is a Lisp macro, not an evaluator-specific shortcut. Keeping
;;; nesting levels explicit prevents inner commas being evaluated too early.
(defun pocket.sys::%qq-rebuild (name value level)
  (list 'list (list 'quote name) (pocket.sys::%qq-expand value level)))
(defun pocket.sys::%qq-expand (form level)
  (cond
    ((simple-vector-p form)
     (list 'apply (list 'function 'vector)
           (pocket.sys::%qq-expand (pocket.sys::%vector-list form 0) level)))
    ((atom form) (list 'quote form))
    ((eq (car form) 'pocket.sys::%unquote)
     (if (= level 1) (cadr form)
         (pocket.sys::%qq-rebuild 'pocket.sys::%unquote (cadr form) (1- level))))
    ((eq (car form) 'pocket.sys::%quasiquote)
     (pocket.sys::%qq-rebuild 'pocket.sys::%quasiquote (cadr form) (1+ level)))
    ((eq (car form) 'pocket.sys::%unquote-splicing)
     (if (= level 1) (pocket.sys::%error "splicing comma outside a list")
         (pocket.sys::%qq-rebuild 'pocket.sys::%unquote-splicing (cadr form) (1- level))))
    ((and (= level 1) (consp (car form))
          (eq (caar form) 'pocket.sys::%unquote-splicing))
     (list 'append (cadar form) (pocket.sys::%qq-expand (cdr form) level)))
    (t (list 'cons (pocket.sys::%qq-expand (car form) level)
             (pocket.sys::%qq-expand (cdr form) level)))))
(pocket.sys::%defmacro 'pocket.sys::%quasiquote
  #'(lambda (form environment)
      (declare (ignore environment))
      (pocket.sys::%qq-expand (cadr form) 1)))

(defun macro-function (name &rest environment)
  (pocket.sys::%macro-function name (if environment (car environment) nil)))
(defun macroexpand-1 (form &rest environment)
  (let* ((env (if environment (car environment) nil))
         (function (if (consp form) (macro-function (car form) env) nil)))
    (if function (values (funcall function form env) t) (values form nil))))
(defun macroexpand (form &rest environment)
  (multiple-value-bind (expanded changed) (apply #'macroexpand-1 form environment)
    (if changed
        (values (apply #'macroexpand expanded environment) t)
        (values form nil))))

(defun pocket.sys::%package-use (package names)
  (if (null names) package
      (progn
        (pocket.sys::%use-package (or (find-package (car names))
                                    (pocket.sys::%error "DEFPACKAGE uses unknown package")) package)
        (pocket.sys::%package-use package (cdr names)))))
(defun pocket.sys::%package-export (package names)
  (if (null names) package
      (progn (pocket.sys::%export (intern (string (car names)) package) package)
             (pocket.sys::%package-export package (cdr names)))))
(defun pocket.sys::%package-shadow (package names)
  (if (null names) package
      (progn (pocket.sys::%shadow (string (car names)) package)
             (pocket.sys::%package-shadow package (cdr names)))))
(defun pocket.sys::%package-options (package options)
  (if (null options) package
      (let ((option (car options)))
        (cond ((eq (car option) :use) (pocket.sys::%package-use package (cdr option)))
              ((eq (car option) :export) (pocket.sys::%package-export package (cdr option)))
              ((eq (car option) :shadow) (pocket.sys::%package-shadow package (cdr option)))
              ((eq (car option) :nicknames) (pocket.sys::%set-nicknames package (mapcar #'string (cdr option))))
              ((eq (car option) :documentation) nil)
              (t (pocket.sys::%error "unsupported DEFPACKAGE option during bootstrap")))
        (pocket.sys::%package-options package (cdr options)))))
(defun pocket.sys::%ensure-package (name options)
  (pocket.sys::%package-options
   (or (find-package name) (pocket.sys::%make-package (string name))) options))
(pocket.sys::%defmacro 'defpackage
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'pocket.sys::%ensure-package (list 'quote (cadr form)) (list 'quote (cddr form)))))
(pocket.sys::%defmacro 'in-package
  #'(lambda (form environment)
      (declare (ignore environment))
      (list 'pocket.sys::%in-package (list 'quote (cadr form)))))

(defun pocket.sys::%proclaim-declaration (declaration)
  (if (eq (car declaration) 'special)
      (mapcar #'pocket.sys::%proclaim-special (cdr declaration)) nil))
(defun proclaim (declaration)
  (pocket.sys::%proclaim-declaration declaration)
  (values))
(pocket.sys::%defmacro 'declaim
  #'(lambda (form environment)
      (declare (ignore environment))
      (cons 'progn
            (mapcar #'(lambda (declaration) (list 'proclaim (list 'quote declaration))) (cdr form)))))
