;;; Development-only inventory tool; normal PocketCL builds use the committed
;;; TSV and require no host Lisp. Run: sbcl --script tools/catalog-symbols.lisp
;;; An external symbol can name both a function and a type (e.g. CONS).
(let ((symbols nil))
  (do-external-symbols (symbol (find-package :cl)) (push symbol symbols))
  (format t "name~Ckind~Cstatus~Cprofiles~%" #\Tab #\Tab #\Tab)
  (dolist (symbol (sort symbols #'string< :key #'symbol-name))
    (let ((kinds nil))
      (cond ((special-operator-p symbol) (push "special-operator" kinds))
            ((macro-function symbol) (push "macro" kinds))
            ((fboundp symbol) (push "function" kinds)))
      (when (boundp symbol)
        (push (if (constantp symbol) "constant" "variable") kinds))
      ;; SUBTYPEP against T is tautologically true even for unknown names in
      ;; some implementations. TYPEP actually resolves the named type.
      (when (handler-bind ((warning #'muffle-warning))
              (handler-case (progn (typep nil symbol) t) (error () nil)))
        (push "type" kinds))
      ;; These names have a syntactic role even where they have no callable
      ;; or type binding. Keep that role visible in the offline inventory.
      (when (member (symbol-name symbol)
                    '("&ALLOW-OTHER-KEYS" "&AUX" "&BODY" "&ENVIRONMENT"
                      "&KEY" "&OPTIONAL" "&REST" "&WHOLE" "DECLARE"
                      "LAMBDA" "SPECIAL" "TYPE" "FTYPE" "INLINE" "NOTINLINE"
                      "IGNORE" "IGNORABLE" "DYNAMIC-EXTENT" "OPTIMIZE" "SPEED"
                      "SPACE" "SAFETY" "DEBUG" "COMPILATION-SPEED")
                    :test #'string=)
        (when (null kinds) (push "symbol" kinds))
        (push "syntax-marker" kinds))
      (format t "~A~C~{~A~^,~}~Cunimplemented~Cnone~%"
              (symbol-name symbol) #\Tab (or (nreverse kinds) '("symbol")) #\Tab #\Tab))))
