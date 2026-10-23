;;; Lexical environments and analysis records for the compiler.
;;; The first record intentionally matches eval0's environment layout: type
;;; -73, slot 0 parent, then VALUE/FUNCTION/MACRO/SYMBOL-MACRO/BLOCK/TAG.
;;; Keeping this shape lets macros pass their &ENVIRONMENT to MACRO-FUNCTION.
(in-package :pocket-compiler)

(defconstant +environment-tag+ -73)
(defconstant +space-value+ 0)
(defconstant +space-function+ 1)
(defconstant +space-macro+ 2)
(defconstant +space-symbol-macro+ 3)
(defconstant +space-block+ 4)
(defconstant +space-tag+ 5)

(defun make-compiler-environment (parent)
  (let ((environment (pocket.sys::%make-record +environment-tag+ 7 nil)))
    (pocket.sys::%set-record-ref environment 0 (if parent parent nil))
    environment))

(defun environment-space (space)
  (cond ((eq space :value) +space-value+)
        ((eq space :function) +space-function+)
        ((eq space :macro) +space-macro+)
        ((eq space :symbol-macro) +space-symbol-macro+)
        ((eq space :block) +space-block+)
        ((eq space :tag) +space-tag+)
        (t (pocket.sys::%error "unknown compiler environment namespace"))))

(defun find-environment-binding (bindings name)
  (if (null bindings) nil
      (if (eq (caar bindings) name) (car bindings)
          (find-environment-binding (cdr bindings) name))))

(defun environment-lookup (environment space name)
  (if (null environment) (values nil nil)
      (let* ((slot (+ 1 (environment-space space)))
             (bindings (pocket.sys::%record-ref environment slot))
             (binding (find-environment-binding bindings name)))
        (if binding (values (cdr binding) t)
            (environment-lookup (pocket.sys::%record-ref environment 0)
                                space name)))))

(defun environment-local-binding (environment space name)
  (find-environment-binding
   (pocket.sys::%record-ref environment (+ 1 (environment-space space))) name))

(defun environment-bind (environment space name value)
  (if (environment-local-binding environment space name)
      (pocket.sys::%error "duplicate binding in compiler environment")
      (pocket.sys::%set-record-ref
       environment (+ 1 (environment-space space))
       (cons (cons name value)
             (pocket.sys::%record-ref environment (+ 1 (environment-space space)))))))

;;; Context and binding vectors are intentionally positional rather than
;;; structures. This compiler runs before DEFSTRUCT and SETF exist.
;;; Context: #(context parent environment locals freevars free-map name depth frame).
;;; Binding: #(binding name namespace owner slot assigned captured boxed kind).
(defconstant +context-parent+ 1)
(defconstant +context-environment+ 2)
(defconstant +context-locals+ 3)
(defconstant +context-freevars+ 4)
(defconstant +context-free-map+ 5)
(defconstant +context-name+ 6)
(defconstant +context-depth+ 7)
(defconstant +context-frame+ 8)

(defconstant +binding-name+ 1)
(defconstant +binding-namespace+ 2)
(defconstant +binding-owner+ 3)
(defconstant +binding-slot+ 4)
(defconstant +binding-assigned+ 5)
(defconstant +binding-captured+ 6)
(defconstant +binding-boxed+ 7)
(defconstant +binding-kind+ 8)

(defun make-compile-context (parent environment name &optional frame)
  (let* ((context (vector :context parent environment (make-buffer) (make-buffer)
                          (make-eq-table) name
                          (if parent (1+ (svref parent +context-depth+)) 0) nil))
         (owner (or frame context)))
    (vset context +context-frame+ owner)
    (if frame
        (progn
          (vset context +context-locals+ (svref frame +context-locals+))
          (vset context +context-freevars+ (svref frame +context-freevars+))
          (vset context +context-free-map+ (svref frame +context-free-map+)))
        nil)
    context))

(defun make-compiler-binding (name namespace owner slot kind)
  (vector :binding name namespace owner slot nil nil nil kind))

(defun compiler-binding-p (value)
  (and (simple-vector-p value) (> (length value) 8)
       (eq (svref value 0) :binding)))

(defun compiler-variable-name-p (name)
  (and (symbolp name) (not (pocket.sys::%constant-symbol-p name))))

(defun context-bind (context name namespace kind)
  (if (and (eq namespace :value) (not (compiler-variable-name-p name)))
      (pocket.sys::%error "cannot bind a constant or non-symbol variable name") nil)
  (let* ((slot (buffer-length (svref context +context-locals+)))
         (binding (make-compiler-binding name namespace context slot kind)))
    (buffer-push (svref context +context-locals+) binding)
    (environment-bind (svref context +context-environment+)
                      namespace name binding)
    binding))

(defun context-temp-bind (context)
  (let* ((slot (buffer-length (svref context +context-locals+)))
         (binding (make-compiler-binding (gensym "COMPILER-TEMP") :temporary
                                         context slot :temporary)))
    (buffer-push (svref context +context-locals+) binding)
    binding))

(defun context-local-count (context)
  (buffer-length (svref context +context-locals+)))

(defun context-free-count (context)
  (buffer-length (svref context +context-freevars+)))

(defun context-free-index (context binding)
  (multiple-value-bind (index found)
      (eq-get (svref context +context-free-map+) binding)
    (if found index
        (let* ((owner (svref binding +binding-owner+))
               (owner-frame (svref owner +context-frame+))
              (parent (svref context +context-parent+)))
          (if (null parent)
              (pocket.sys::%error "compiler free variable has no parent context")
              (progn
                ;; Ensure every intervening closure relays a grandparent's
                ;; value, so all runtime environments stay flat.
                (if (eq owner-frame (svref parent +context-frame+)) nil
                    (context-free-index parent binding))
                (let* ((frame (svref context +context-frame+))
                       (slot (buffer-push (svref frame +context-freevars+)
                                          binding)))
                  (eq-put (svref frame +context-free-map+) binding slot)
                  (vset binding +binding-captured+ t)
                  slot)))))))

(defun context-resolve-binding (context binding)
  (let ((owner (svref binding +binding-owner+)))
    (if (eq (svref owner +context-frame+) (svref context +context-frame+))
        (values :local (svref binding +binding-slot+))
        (values :free (context-free-index context binding)))))

(defun context-lookup (context name namespace)
  (multiple-value-bind (binding found)
      (environment-lookup (svref context +context-environment+) namespace name)
    (if (and found (compiler-binding-p binding))
        (values binding t)
        (values nil nil))))

(defun context-add-special-marker (context name)
  (let ((local (environment-local-binding
                (svref context +context-environment+) :value name)))
    (if local nil
        (environment-bind (svref context +context-environment+)
                          :value name :special-marker))))

(defun context-mark-assigned (binding)
  (vset binding +binding-assigned+ t)
  binding)

(defun context-mark-boxed (binding)
  (vset binding +binding-boxed+ t)
  binding)
