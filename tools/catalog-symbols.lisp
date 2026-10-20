;;; Development-only inventory tool; normal PocketCL builds use the committed
;;; TSV and require no host Lisp. Run: sbcl --script tools/catalog-symbols.lisp
;;; An external symbol can name both a function and a type (e.g. CONS).
(let ((symbols nil))
  (do-external-symbols (symbol (find-package :cl)) (push symbol symbols))
  (format t "name~Ckind~Cstatus~%" #\Tab #\Tab)
  (dolist (symbol (sort symbols #'string< :key #'symbol-name))
    (let ((kinds nil))
      (cond ((special-operator-p symbol) (push "special-operator" kinds))
            ((macro-function symbol) (push "macro" kinds))
            ((fboundp symbol) (push "function" kinds)))
      (when (boundp symbol)
        (push (if (constantp symbol) "constant" "variable") kinds))
      (when (handler-bind ((warning #'muffle-warning))
              (ignore-errors (nth-value 1 (subtypep symbol t))))
        (push "type" kinds))
      (format t "~A~C~{~A~^,~}~Cunimplemented~%"
              (symbol-name symbol) #\Tab (or (nreverse kinds) '("symbol")) #\Tab))))
