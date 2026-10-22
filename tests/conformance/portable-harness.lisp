;;; The same test source runs on PocketCL and SBCL. Reader conditionals select
;;; only reporting/error plumbing; all assertions use portable CL semantics.
(defpackage :pocket.test
  (:use :cl)
  (:export #:deftest #:is #:is-equal #:finish #:*assertions*))
(in-package :pocket.test)
(defvar *assertions* 0)
(defvar *test-name* nil)
(defmacro deftest (name &body body)
  (list 'let (list (list '*test-name* (list 'quote name))) (cons 'progn body)))
(defmacro is (expression)
  (list 'progn
        (list 'setq '*assertions* (list '1+ '*assertions*))
        (list 'if expression t
              #+pocket
              (list 'pocket.sys::%error
                    (list 'pocket.sys::%join-strings
                          "Conformance assertion failed in "
                          (list 'symbol-name '*test-name*)))
              #-pocket (list 'error "Conformance assertion failed in ~S: ~S" '*test-name* (list 'quote expression)))))
(defmacro is-equal (expected actual)
  (list 'is (list 'equal expected actual)))
(defun finish ()
  #+pocket
  (pocket.sys::%console-write-string
   (pocket.sys::%join-strings
    (pocket.sys::%join-strings "Conformance: " (pocket.sys::%integer-string *assertions*))
    (string #\Newline)))
  #-pocket (format t "Conformance: ~D~%" *assertions*)
  *assertions*)
