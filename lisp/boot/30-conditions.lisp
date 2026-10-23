;;; M3 needs one small part of the later condition system: ordinary Lisp
;;; handlers must be able to escape a VM stack-overflow before its frames are
;;; unwound. Conditions are tagged vectors for now; M4 supplies the complete
;;; type, condition-definition, debugger and restart interfaces.
(defpackage :pocket (:use :cl) (:export #:stack-overflow #:heap-exhausted))
(in-package :cl)
(defvar pocket.sys::*handler-clusters* nil)

(defun pocket.sys::%condition (kind datum expected message)
  (vector 'pocket.sys::%condition kind datum expected message))
(defun pocket.sys::%condition-p (object)
  (and (simple-vector-p object) (= (length object) 5)
       (eq (svref object 0) 'pocket.sys::%condition)))
(defun pocket.sys::%known-condition-name-p (name)
  (member name '(condition serious-condition error simple-condition simple-error
                 type-error unbound-variable undefined-function
                 pocket:stack-overflow pocket:heap-exhausted)))
(defun pocket.sys::%condition-matches (condition type)
  (let ((kind (svref condition 1)))
    (cond ((eq type t) t)
          ((eq type kind) t)
          ((eq type 'condition) t)
          ((eq type 'serious-condition)
           (not (member kind '(condition simple-condition))))
          ((eq type 'error)
           (not (member kind '(condition serious-condition simple-condition))))
          ((and (consp type) (eq (car type) 'or))
           (pocket.sys::%condition-any condition (cdr type)))
          (t nil))))
(defun pocket.sys::%condition-any (condition types)
  (if (null types) nil
      (or (pocket.sys::%condition-matches condition (car types))
          (pocket.sys::%condition-any condition (cdr types)))))
(defun pocket.sys::%condition-designator (datum default arguments)
  (cond ((pocket.sys::%condition-p datum)
         (if arguments (pocket.sys::%error "condition object takes no arguments") datum))
        ((stringp datum)
         (if arguments (pocket.sys::%error "condition formatting arrives at M4")
             (pocket.sys::%condition default nil nil datum)))
        ((symbolp datum)
         (if arguments (pocket.sys::%error "condition initargs arrive at M4")
             (if (pocket.sys::%known-condition-name-p datum)
                 (pocket.sys::%condition datum nil nil nil)
                 (pocket.sys::%error "condition type is not implemented in the M3 subset"))))
        (t (pocket.sys::%error "unsupported condition designator"))))
(defun pocket.sys::%dispatch-condition (condition clusters)
  (if (null clusters) nil
      (progn
        ;; Every handler in this cluster sees only outer clusters. Declining a
        ;; condition resumes the search in source order; a THROW/GO escapes.
        (let ((pocket.sys::*handler-clusters* (cdr clusters))
              (handlers (car clusters)))
          (tagbody next
            (if (null handlers) (go done) nil)
            (if (pocket.sys::%condition-matches condition (caar handlers))
                (funcall (cdar handlers) condition) nil)
            (setq handlers (cdr handlers))
            (go next)
           done))
        (pocket.sys::%dispatch-condition condition (cdr clusters)))))
(defun signal (datum &rest arguments)
  (pocket.sys::%dispatch-condition
   (pocket.sys::%condition-designator datum 'simple-condition arguments)
   pocket.sys::*handler-clusters*)
  nil)
(defun error (datum &rest arguments)
  (let ((condition (pocket.sys::%condition-designator datum 'simple-error arguments)))
    (signal condition)
    ;; A declined Lisp condition has already visited every handler. Mark its
    ;; host error so the VM does not signal it a second time through the hook.
    (let ((message (or (svref condition 4) "unhandled Lisp condition")))
      (if (fboundp 'pocket.sys::%unhandled-error)
          (pocket.sys::%unhandled-error condition message)
          (pocket.sys::%error message)))))
(defun pocket.sys::%handler-bindings (bindings)
  (if (null bindings) nil
      (cons (list 'cons (list 'quote (caar bindings)) (cadar bindings))
            (pocket.sys::%handler-bindings (cdr bindings)))))
(defmacro handler-bind (bindings &body body)
  (cons 'let
        (cons (list (list 'pocket.sys::*handler-clusters*
                          (list 'cons (cons 'list (pocket.sys::%handler-bindings bindings))
                                'pocket.sys::*handler-clusters*)))
              body)))

;;; HANDLER-CASE uses lexical GO targets. The handler only saves the condition
;;; and transfers; the user's clause runs after dynamic bindings and unwind
;;; cleanups have been restored. This is also a useful compiler closure test.
(defun pocket.sys::%case-handlers (clauses labels condition)
  (if (null clauses) nil
      (cons (list (caar clauses)
                  (list 'function
                        (list 'lambda (list condition)
                              (list 'setq (car labels) condition)
                              (list 'go (cadr labels)))))
            (pocket.sys::%case-handlers (cdr clauses) (cddr labels) condition))))
(defun pocket.sys::%case-labels (clauses)
  (if (null clauses) nil
      (cons (gensym "CONDITION")
            (cons (gensym "CLAUSE") (pocket.sys::%case-labels (cdr clauses))))))
(defun pocket.sys::%case-slots (labels)
  (if (null labels) nil
      (cons (list (car labels) nil) (pocket.sys::%case-slots (cddr labels)))))
(defun pocket.sys::%case-bodies (clauses labels exit)
  (if (null clauses) nil
      (let ((clause (car clauses)))
        (cons (cadr labels)
              (cons (list 'return-from exit
                          ;; A lambda preserves the declaration prefix and
                          ;; accepts either the optional condition or nothing.
                          (cons 'funcall
                                (cons (list 'function
                                            (cons 'lambda (cons (cadr clause) (cddr clause))))
                                      (if (cadr clause) (list (car labels)) nil))))
                    (pocket.sys::%case-bodies (cdr clauses) (cddr labels) exit))))))
(defun pocket.sys::%case-no-error (clauses)
  (if (null clauses) nil
      (if (eq (caar clauses) :no-error) (car clauses)
          (pocket.sys::%case-no-error (cdr clauses)))))
(defun pocket.sys::%case-errors (clauses)
  (if (null clauses) nil
      (if (eq (caar clauses) :no-error) (pocket.sys::%case-errors (cdr clauses))
          (cons (car clauses) (pocket.sys::%case-errors (cdr clauses))))))
(defmacro handler-case (expression &rest clauses)
  (let ((no-error (pocket.sys::%case-no-error clauses))
        (errors (pocket.sys::%case-errors clauses))
        (exit (gensym "HANDLED")) (normal (gensym "NORMAL")))
    (if no-error
        (list 'block exit
              (list 'multiple-value-call
                    (list 'function (cons 'lambda (cdr no-error)))
                    (list 'block normal
                          (list 'return-from exit
                                (cons 'handler-case
                                      (cons (list 'return-from normal expression) errors))))))
        (let ((labels (pocket.sys::%case-labels errors))
              (condition (gensym "CAUGHT")))
          (list 'block exit
                (list 'let (pocket.sys::%case-slots labels)
                      (cons 'tagbody
                            (cons (list 'return-from exit
                                        (list 'handler-bind
                                              (pocket.sys::%case-handlers errors labels condition)
                                              expression))
                                  (pocket.sys::%case-bodies errors labels exit)))))))))

;;; Numeric codes are the stable kernel error vocabulary in pocket.h. The VM
;;; calls this as an ordinary closure with emergency stack capacity, keeping
;;; the faulting dynamic state live so a handler can perform a nonlocal exit.
(defun pocket.sys::%signal-error (code datum expected)
  (signal (pocket.sys::%condition
           (cond ((= code 11) 'pocket:stack-overflow)
                 ((= code 1) 'pocket:heap-exhausted)
                 ((= code 2) 'type-error)
                 ((= code 8) 'unbound-variable)
                 ((= code 9) 'undefined-function)
                 (t 'error))
           datum expected nil)))
(when (fboundp 'pocket.sys::%install-hooks)
  (pocket.sys::%install-hooks #'pocket.sys::%signal-error))
(in-package :cl)
