;;; Macro expansion and the small pieces of lexical analysis shared by
;;; codegen. Forms remain ordinary Lisp trees until a function body is emitted.
(in-package :pocket-compiler)

(defun proper-form-list-p (items)
  (if (null items) t
      (if (consp items) (proper-form-list-p (cdr items)) nil)))

(defun compiler-form-name-p (form name)
  (and (consp form) (eq (car form) name)))

(defun declaration-form-p (form)
  (compiler-form-name-p form 'declare))

(defun body-declarations (body)
  (if (and (consp body) (declaration-form-p (car body)))
      (cons (car body) (body-declarations (cdr body)))
      nil))

(defun body-expressions (body)
  (if (and (consp body) (declaration-form-p (car body)))
      (body-expressions (cdr body))
      body))

(defun declared-special-p (name declarations)
  (if (null declarations) nil
      (or (declaration-spec-special-p name (cdar declarations))
          (declared-special-p name (cdr declarations)))))
(defun declaration-spec-special-p (name specs)
  (if (null specs) nil
      (or (and (consp (car specs)) (eq (caar specs) 'special)
               (member name (cdar specs)))
          (declaration-spec-special-p name (cdr specs)))))

(defun symbol-globally-special-p (name)
  (pocket.sys::%special-p name))

(defun compiler-special-p (name declarations environment)
  (or (declared-special-p name declarations)
      (symbol-globally-special-p name)
      (multiple-value-bind (marker found)
          (environment-lookup environment :value name)
        (and found (eq marker :special-marker)))))

(defun compiler-macro-function (name environment)
  (if (not (symbolp name)) nil
      (let ((function-binding (environment-local-binding environment :function name))
            (macro-binding (environment-local-binding environment :macro name)))
        (cond (function-binding nil)
              (macro-binding (cdr macro-binding))
              ((pocket.sys::%record-ref environment 0)
               (compiler-macro-function name (pocket.sys::%record-ref environment 0)))
              (t (macro-function name environment))))))

(defun compiler-symbol-macro (name environment)
  (let ((value-binding (environment-local-binding environment :value name))
        (macro-binding (environment-local-binding environment :symbol-macro name)))
    (cond (value-binding nil)
          (macro-binding (values (cdr macro-binding) t))
          ((pocket.sys::%record-ref environment 0)
           (compiler-symbol-macro name (pocket.sys::%record-ref environment 0)))
          (t (values nil nil)))))

(defun compiler-core-form-p (name)
  (compiler-core-form-tail-p name
    '(quote lambda function if progn setq let let* block return-from catch
      throw unwind-protect tagbody go values multiple-value-call
      multiple-value-prog1 multiple-value-list locally eval-when macrolet
      symbol-macrolet the)))
(defun compiler-core-form-tail-p (name names)
  (if (null names) nil
      (if (eq name (car names)) t
          (compiler-core-form-tail-p name (cdr names)))))

(defun expand-form-once (form environment)
  (if (symbolp form)
      (multiple-value-bind (expansion found)
          (compiler-symbol-macro form environment)
        (if found (values expansion t) (values form nil)))
      (if (not (consp form)) (values form nil)
          (if (compiler-core-form-p (car form))
              (values form nil)
              (let ((transformer (compiler-macro-function (car form) environment)))
                (if transformer
                    (values (funcall transformer form environment) t)
                    (values form nil)))))))

(defun expand-form (form environment)
  (let ((steps 0))
    (tagbody again
      (if (> steps 256) (pocket.sys::%error "macro expansion limit exceeded") nil)
      (multiple-value-bind (next changed) (expand-form-once form environment)
        (if changed
            (progn (setq form next) (setq steps (1+ steps)) (go again)) nil)))
    form))

(defun expand-subforms (forms environment)
  (if (null forms) nil
      (cons (expand-form (car forms) environment)
            (expand-subforms (cdr forms) environment))))

;;; Analysis database tables are identity maps keyed by source conses. The
;;; analysis pass creates every frame slot and records expansion results before
;;; codegen starts, so a later nested assignment cannot change earlier code.
;;; Layout: #(contexts expansions lambda-data let-data root-context).
(defconstant +analysis-contexts+ 0)
(defconstant +analysis-expansions+ 1)
(defconstant +analysis-lambdas+ 2)
(defconstant +analysis-lets+ 3)
(defconstant +analysis-root+ 4)

(defconstant +analysis-blocks+ 5)
(defconstant +analysis-tagbodies+ 6)
(defconstant +analysis-mvcalls+ 7)
(defconstant +analysis-mvprog1+ 8)
(defconstant +analysis-unwinds+ 9)
(defconstant +analysis-funs+ 10)
(defun make-analysis-database (root)
  (vector (make-eq-table) (make-eq-table) (make-eq-table)
          (make-eq-table) root (make-eq-table) (make-eq-table)
          (make-eq-table) (make-eq-table) (make-eq-table)
          (make-eq-table)))

;;; A source cons can be reused at multiple occurrences with different lexical
;;; parents. Keep every analysis record behind both identities, just like the
;;; expansion cache, so later visits cannot overwrite the first contour.
(defun analysis-table-get (database slot context form)
  (multiple-value-bind (entries found)
      (eq-get (svref database slot) context)
    (if found (eq-get entries form) (values nil nil))))
(defun analysis-table-put (database slot context form value)
  (let ((table (svref database slot)))
    (multiple-value-bind (entries found) (eq-get table context)
      (if found nil
          (progn (setq entries (make-eq-table)) (eq-put table context entries)))
      (eq-put entries form value))))

(defun analysis-record-expansion (database context form expanded)
  (let* ((table (svref database +analysis-expansions+))
         (occurrences (multiple-value-bind (value found) (eq-get table context)
                        (if found value
                            (let ((value (make-eq-table)))
                              (eq-put table context value)
                              value)))))
    (eq-put occurrences form expanded)))

(defun analysis-expanded-form (database form context)
  (let ((occurrences (multiple-value-bind (value found)
                         (eq-get (svref database +analysis-expansions+) context)
                       (if found value nil))))
    (if occurrences
        (multiple-value-bind (expanded found) (eq-get occurrences form)
          (if found expanded form))
        form)))

(defun analysis-context-for (database form fallback)
  (multiple-value-bind (context found)
      (analysis-table-get database +analysis-contexts+ fallback form)
    (if found context fallback)))

(defun analysis-body (database forms context declarations)
  (if (null forms) nil
      (progn (analysis-form database (car forms) context declarations)
             (analysis-body database (cdr forms) context declarations))))

(defun analysis-finalize-frame (context)
  (let ((frame (svref context +context-frame+)) (index 0)
        (locals (svref (svref context +context-frame+) +context-locals+)))
    (tagbody each-local
      (if (= index (buffer-length locals)) (go done) nil)
      (let ((binding (buffer-ref locals index)))
        (if (and (svref binding +binding-assigned+)
                 (svref binding +binding-captured+))
            (context-mark-boxed binding) nil))
      (setq index (1+ index)) (go each-local)
     done)
    frame))

(defun analysis-bind-parameters (context parameters declarations)
  (let ((required (make-buffer)) (rest-name nil) (cursor parameters))
    (tagbody parse
      (cond ((null cursor) (go done))
            ((and (consp cursor) (eq (car cursor) '&rest))
             (if (and (consp (cdr cursor)) (null (cddr cursor)))
                 (progn (setq rest-name (cadr cursor)) (go done))
                 (pocket.sys::%error "malformed &REST lambda list")))
            ((consp cursor)
             (if (symbolp (car cursor))
                 (progn (buffer-push required (car cursor))
                        (setq cursor (cdr cursor)) (go parse))
                 (pocket.sys::%error "compiler requires required parameters")))
            (t (pocket.sys::%error "dotted lambda lists are unsupported")))
     done)
    (let ((index 0))
      (tagbody req
        (if (= index (buffer-length required)) (go req-done) nil)
        (let* ((name (buffer-ref required index))
               (binding (context-bind context name :value :parameter)))
          (if (or (declared-special-p name declarations)
                  (symbol-globally-special-p name))
              (vset binding +binding-kind+ :special) nil))
        (setq index (1+ index)) (go req)
       req-done))
    (if rest-name
        (let ((binding (context-bind context rest-name :value :parameter)))
          (if (or (declared-special-p rest-name declarations)
                  (symbol-globally-special-p rest-name))
              (vset binding +binding-kind+ :special) nil)) nil)
    (vector (buffer-length required) (if rest-name t nil) rest-name)))

(defun analysis-mark-declared-specials (context declarations)
  (if (null declarations) nil
      (progn
        (analysis-mark-special-specs context (cdar declarations))
        (analysis-mark-declared-specials context (cdr declarations)))))
(defun analysis-mark-special-specs (context specs)
  (if (null specs) nil
      (progn
        (if (and (consp (car specs)) (eq (caar specs) 'special))
            (analysis-mark-special-names context (cdar specs)) nil)
        (analysis-mark-special-specs context (cdr specs)))))
(defun analysis-mark-special-names (context names)
  (if (null names) nil
      (progn
        ;; A SPECIAL declaration applies in this contour. Mutating a binding
        ;; found through an outer environment would make the declaration leak
        ;; into unrelated code that shares the same frame.
        (let ((local (environment-local-binding
                      (svref context +context-environment+) :value (car names))))
          (if (and local (compiler-binding-p (cdr local)))
              (vset (cdr local) +binding-kind+ :special)
              (context-add-special-marker context (car names))))
        (analysis-mark-special-names context (cdr names)))))

(defun analysis-setq-symbol-macro-pairs (pairs environment)
  (if (null pairs) (values nil nil)
      (multiple-value-bind (target expanded)
          (compiler-symbol-macro (car pairs) environment)
        (if (and expanded (not (symbolp target)))
            (pocket.sys::%error "SETQ requires a simple symbol-macro place")
            (multiple-value-bind (tail tail-expanded)
                (analysis-setq-symbol-macro-pairs (cddr pairs) environment)
              (values (cons (if expanded target (car pairs))
                            (cons (cadr pairs) tail))
                      (or expanded tail-expanded)))))))

(defun analysis-lambda (database form context)
  (let* ((parameters (cadr form)) (body (cddr form))
         (declarations (body-declarations body))
         (environment (make-compiler-environment
                       (svref context +context-environment+)))
         (child (make-compile-context context environment nil)))
    (analysis-table-put database +analysis-contexts+ context form child)
    (let ((parameters-data (analysis-bind-parameters child parameters declarations)))
      (analysis-table-put database +analysis-lambdas+ context form parameters-data))
    (analysis-mark-declared-specials child declarations)
    (analysis-body database (body-expressions body) child declarations)
    (analysis-finalize-frame child)
    nil))

(defun compiler-normalize-lambda (form)
  (let ((parameters (cadr form)) (body (cddr form)))
    (if (pocket.sys::%rich-lambda-p parameters)
        (pocket.sys::%expand-lambda parameters body)
        form)))

(defun analysis-let (database form context sequential)
  (let* ((bindings (cadr form)) (body (cddr form))
         (declarations (body-declarations body))
         (scope (make-compile-context context
                  (make-compiler-environment (svref context +context-environment+))
                  nil (svref context +context-frame+)))
         (entries (make-buffer)) (cursor bindings))
    (if (not (proper-form-list-p bindings))
        (pocket.sys::%error "LET binding list must be proper") nil)
    (tagbody gather
      (if (null cursor) (go gathered) nil)
      (let* ((spec (car cursor))
             (name (if (consp spec) (car spec) spec))
             (init (if (and (consp spec) (consp (cdr spec))) (cadr spec) nil))
             (init-context (if sequential scope context)))
        (if (not (symbolp name)) (pocket.sys::%error "LET binding name must be a symbol") nil)
        (analysis-form database init init-context nil)
        (if (and sequential (not (eq init-context scope))) nil
            (if sequential
                (setq scope (make-compile-context scope
                          (make-compiler-environment (svref scope +context-environment+))
                          nil (svref context +context-frame+))) nil))
        (let* ((binding (context-bind scope name :value :lexical))
               (temporary (if sequential nil (context-temp-bind scope))))
          (if (or (declared-special-p name declarations)
                  (symbol-globally-special-p name))
              (vset binding +binding-kind+ :special) nil)
          (buffer-push entries (vector init init-context binding temporary))))
      (setq cursor (cdr cursor)) (go gather)
     gathered)
    (analysis-table-put database +analysis-contexts+ context form scope)
    (analysis-table-put database +analysis-lets+ context form (buffer-vector entries))
    (analysis-mark-declared-specials scope declarations)
    (analysis-body database (body-expressions body) scope declarations)
    nil))

(defun analysis-tagbody (database form context declarations)
  (let* ((forms (cdr form))
         (scope (make-compile-context context
                  (make-compiler-environment (svref context +context-environment+))
                  nil (svref context +context-frame+)))
         (token-name (gensym "TAGBODY-TOKEN"))
         (token (context-bind scope token-name :block :tagbody-token))
         (tags (make-buffer)) (items forms) (label 0))
    (tagbody scan-tags
      (if (null items) (go tags-scanned) nil)
      (if (consp (car items)) nil
          (let ((tag (vector :tag token label)))
            (environment-bind (svref scope +context-environment+) :tag (car items) tag)
            (buffer-push tags (vector (car items) label))))
      (if (consp (car items)) nil (setq label (1+ label)))
      (setq items (cdr items)) (go scan-tags)
     tags-scanned)
    ;; The tag pass walks the whole body to assign stable label indices. Start
    ;; from the original list again so every form receives binding analysis.
    (setq items forms)
    (tagbody analyze-items
      (if (null items) (go items-analyzed) nil)
      (if (consp (car items))
          (analysis-form database (car items) scope declarations) nil)
      (setq items (cdr items)) (go analyze-items)
     items-analyzed)
    (analysis-table-put database +analysis-contexts+ context form scope)
    (analysis-table-put database +analysis-tagbodies+ context form
                        (vector forms scope token (buffer-vector tags)))
    nil))

;;; Local macro transformers execute during expansion. They are compiled as
;;; closed core lambdas; this keeps the environment passed to MACRO-FUNCTION
;;; compatible with the interpreter record without capturing runtime locals.
(defun analysis-macrolet (database form context declarations)
  (let* ((environment (make-compiler-environment
                       (svref context +context-environment+)))
         (scope (make-compile-context context environment nil
                                      (svref context +context-frame+)))
         (definitions (cadr form)))
    (if (not (proper-form-list-p definitions))
        (pocket.sys::%error "MACROLET definitions must be a proper list") nil)
    (tagbody install
      (if (null definitions) (go installed) nil)
      (let* ((definition (car definitions))
             (name (car definition))
             ;; MACROLET lambda lists describe the arguments after the macro
             ;; name. Expansion supplies the whole form and environment, so
             ;; normalize them through the same helper used by DEFMACRO.
             (lambda-form
               (pocket.sys::%expand-macro-lambda
                (cadr definition)
                (pocket.sys::%named-body name (cddr definition))))
             (proto (compile-form lambda-form))
             (transformer (execute-proto proto)))
        (if (not (symbolp name)) (pocket.sys::%error "MACROLET name must be a symbol") nil)
        (environment-bind environment :macro name transformer))
      (setq definitions (cdr definitions))
      (go install)
     installed)
    (analysis-table-put database +analysis-contexts+ context form scope)
    (analysis-body database (cddr form) scope declarations)
    nil))

(defun analysis-symbol-macrolet (database form context declarations)
  (let* ((environment (make-compiler-environment
                       (svref context +context-environment+)))
         (scope (make-compile-context context environment nil
                                      (svref context +context-frame+)))
         (definitions (cadr form)))
    (if (not (proper-form-list-p definitions))
        (pocket.sys::%error "SYMBOL-MACROLET definitions must be a proper list") nil)
    (tagbody install
      (if (null definitions) (go installed) nil)
      (let ((definition (car definitions)))
        (if (or (not (consp definition)) (not (symbolp (car definition)))
                (not (consp (cdr definition))) (cddr definition)
                (not (compiler-variable-name-p (car definition))))
            (pocket.sys::%error "malformed SYMBOL-MACROLET binding") nil)
        (environment-bind environment :symbol-macro (car definition)
                          (cadr definition)))
      (setq definitions (cdr definitions))
      (go install)
     installed)
    (analysis-table-put database +analysis-contexts+ context form scope)
    (analysis-body database (cddr form) scope declarations)
    nil))

;;; Local functions occupy ordinary frame slots but use the FUNCTION
;;; namespace. LABELS preallocates mutable cells so mutually recursive
;;; closures can capture one another before their values are installed.
(defun analysis-local-functions (database form context labels-p)
  (let* ((environment (make-compiler-environment
                       (svref context +context-environment+)))
         (scope (make-compile-context context environment nil
                                      (svref context +context-frame+)))
         (definitions (cadr form)) (entries (make-buffer)))
    (if (not (proper-form-list-p definitions))
        (pocket.sys::%error "local function definitions must be a proper list") nil)
    (if labels-p
        (tagbody bind-names
          (if (null definitions) (go names-bound) nil)
          (let* ((definition (car definitions)) (name (car definition))
                 (binding (context-bind scope name :function :labels)))
            (context-mark-boxed binding)
            (buffer-push entries
                         (vector name
                                 (compiler-normalize-lambda
                                  (cons 'lambda
                                        (cons (cadr definition)
                                              (pocket.sys::%named-body
                                               name (cddr definition)))))
                                 binding)))
          (setq definitions (cdr definitions)) (go bind-names)
         names-bound)
        nil)
    (setq definitions (cadr form))
    (tagbody bind-flet-names
      (if (or labels-p (null definitions)) (go flet-names-bound) nil)
      (let* ((definition (car definitions)) (name (car definition))
             (binding (context-bind scope name :function :flet)))
        (buffer-push entries
                     (vector name
                             (compiler-normalize-lambda
                              (cons 'lambda
                                    (cons (cadr definition)
                                          (pocket.sys::%named-body
                                           name (cddr definition)))))
                             binding)))
      (setq definitions (cdr definitions)) (go bind-flet-names)
     flet-names-bound)
    ;; FLET definitions are analyzed outside the local function scope;
    ;; LABELS definitions see every binding introduced above.
    (let ((index 0) (functions (buffer-vector entries)))
      (tagbody analyze-definitions
        (if (= index (length functions)) (go definitions-analyzed) nil)
        (let ((lambda-form (svref (svref functions index) 1)))
          (analysis-form database lambda-form (if labels-p scope context) nil))
        (setq index (1+ index)) (go analyze-definitions)
       definitions-analyzed)
      (analysis-table-put database +analysis-contexts+ context form scope)
      (analysis-table-put database +analysis-funs+ context form
                          (vector labels-p functions)))
    (analysis-body database (cddr form) scope nil)
    nil))

(defun analysis-variable-reference (database form context declarations)
  (multiple-value-bind (expanded symbol-macro-p)
      (compiler-symbol-macro form (svref context +context-environment+))
    (if symbol-macro-p
        (progn
          (analysis-record-expansion database context form expanded)
          (analysis-form database expanded context declarations))
        (multiple-value-bind (binding found) (context-lookup context form :value)
          (if (and found (compiler-binding-p binding)
                   (not (eq (svref binding +binding-kind+) :special)))
              (context-resolve-binding context binding) nil)))))

(defun analysis-form (database source context declarations)
  (let ((form source))
    (if (symbolp form)
        (analysis-variable-reference database form context declarations)
        (if (not (consp form)) nil
            (let ((expanded (expand-form form (svref context +context-environment+))))
              (if (not (eq expanded form))
                  (progn (analysis-record-expansion database context form expanded)
                         (analysis-form database expanded context declarations))
                  (cond
                    ((eq (car form) 'quote) nil)
                    ((eq (car form) 'lambda)
                     (let ((normalized (compiler-normalize-lambda form)))
                       (if (eq normalized form)
                           (analysis-lambda database form context)
                           (progn
                             (analysis-record-expansion database context form normalized)
                             (analysis-form database normalized context declarations)))))
                    ((eq (car form) 'let) (analysis-let database form context nil))
                    ((eq (car form) 'let*) (analysis-let database form context t))
                    ((eq (car form) 'function)
                     (let ((designator (cadr form)))
                       (if (and (consp designator) (eq (car designator) 'lambda))
                           (analysis-form database designator context declarations)
                           (if (symbolp designator)
                               (multiple-value-bind (binding found)
                                   (context-lookup context designator :function)
                                 (if (and found (compiler-binding-p binding))
                                     (context-resolve-binding context binding) nil)) nil))))
                    ((eq (car form) 'setq)
                     (multiple-value-bind (pairs expanded)
                         (analysis-setq-symbol-macro-pairs
                          (cdr form) (svref context +context-environment+))
                       (if expanded
                           (let ((replacement (cons 'setq pairs)))
                             (analysis-record-expansion database context form replacement)
                             (analysis-form database replacement context declarations))
                           (tagbody each-pair
                             (if (null pairs) (go done-pairs) nil)
                             (analysis-form database (cadr pairs) context declarations)
                             (multiple-value-bind (binding found)
                                 (context-lookup context (car pairs) :value)
                               (if (and found (compiler-binding-p binding)
                                        (not (eq (svref binding +binding-kind+) :special)))
                                   (progn (context-mark-assigned binding)
                                          (context-resolve-binding context binding)) nil))
                             (setq pairs (cddr pairs)) (go each-pair)
                            done-pairs))))
                    ((eq (car form) 'multiple-value-call)
                     (analysis-form database (cadr form) context declarations)
                     (let ((function-temp (context-temp-bind context))
                           (argument-temps (make-buffer))
                           (arguments (cddr form)))
                       (tagbody each-argument
                         (if (null arguments) (go arguments-done) nil)
                         (analysis-form database (car arguments) context declarations)
                         (buffer-push argument-temps (context-temp-bind context))
                         (setq arguments (cdr arguments))
                         (go each-argument)
                        arguments-done)
                       (analysis-table-put database +analysis-mvcalls+ context form
                                           (vector function-temp
                                                   (buffer-vector argument-temps)))))
                    ((eq (car form) 'multiple-value-prog1)
                     (analysis-form database (cadr form) context declarations)
                     (analysis-body database (cddr form) context declarations)
                     (analysis-table-put database +analysis-mvprog1+ context form
                                         (context-temp-bind context)))
                    ((eq (car form) 'unwind-protect)
                     (let ((cleanup-lambda (cons 'lambda
                                                 (cons nil (cddr form)))))
                       (analysis-form database (cadr form) context declarations)
                       (analysis-form database cleanup-lambda context declarations)
                       (analysis-table-put database +analysis-unwinds+ context form
                                           cleanup-lambda)))
                    ((eq (car form) 'macrolet)
                     (analysis-macrolet database form context declarations))
                    ((eq (car form) 'symbol-macrolet)
                     (analysis-symbol-macrolet database form context declarations))
                    ((eq (car form) 'flet)
                     (analysis-local-functions database form context nil))
                    ((eq (car form) 'labels)
                     (analysis-local-functions database form context t))
                    ((eq (car form) 'catch)
                     (analysis-form database (cadr form) context declarations)
                     (analysis-body database (cddr form) context declarations))
                    ((eq (car form) 'throw)
                     (analysis-form database (cadr form) context declarations)
                     (analysis-form database (caddr form) context declarations))
                    ((eq (car form) 'block)
                     ;; Analyze exactly once under a real lexical token. As
                     ;; RETURN-FROM forms are macroexpanded and visited, they
                     ;; mark the resolved token as used. This avoids walking
                     ;; cons-list tails as though they were executable forms,
                     ;; respects shadowing, and still lets codegen omit a
                     ;; block that cannot be exited from its body.
                     (let* ((name (cadr form))
                            (environment (make-compiler-environment
                                          (svref context +context-environment+)))
                            (scope (make-compile-context context environment nil
                                                         (svref context +context-frame+)))
                            (binding (context-bind scope name :block :block-token))
                            (body (cddr form)))
                       (analysis-table-put database +analysis-contexts+ context form scope)
                       (analysis-body database body scope declarations)
                       (analysis-table-put database +analysis-blocks+ context form
                                           (vector body
                                                   (svref binding +binding-assigned+)
                                                   binding))))
                    ((eq (car form) 'return-from)
                     (progn
                       (multiple-value-bind (binding found)
                           (context-lookup context (cadr form) :block)
                         (if (and found (compiler-binding-p binding))
                             (progn (context-mark-assigned binding)
                                    (context-resolve-binding context binding))
                             (pocket.sys::%error "RETURN-FROM has no lexical BLOCK")))
                       (analysis-form database (caddr form) context declarations)))
                    ((eq (car form) 'go)
                     (multiple-value-bind (tag found)
                         (environment-lookup (svref context +context-environment+)
                                             :tag (cadr form))
                       (if (and found (simple-vector-p tag)
                                (eq (svref tag 0) :tag))
                           (context-resolve-binding context (svref tag 1))
                           (pocket.sys::%error "GO has no lexical TAGBODY tag"))))
                    ((eq (car form) 'tagbody)
                     (analysis-tagbody database form context declarations))
                    ((eq (car form) 'declare) nil)
                    ((eq (car form) 'locally)
                     (let* ((body (cdr form)) (decls (body-declarations body))
                            (scope (make-compile-context context
                                      (make-compiler-environment
                                       (svref context +context-environment+))
                                      nil (svref context +context-frame+))))
                       (analysis-table-put database +analysis-contexts+ context form scope)
                       (analysis-mark-declared-specials scope decls)
                       (analysis-body database (body-expressions body) scope decls)))
                    ((eq (car form) 'eval-when)
                     (if (member :execute (cadr form))
                         (analysis-body database (cddr form) context declarations)
                         nil))
                    (t
                     (analysis-form database (car form) context declarations)
                     (analysis-body database (cdr form) context declarations)))))))))
