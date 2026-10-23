;;; These tests need the compiler support primitives and files 00, 01 and 50,
;;; but do not need the compiler itself. This keeps the wire-format boundary
;;; testable even while changing the compiler's analysis or code generator.
(in-package :pocket.test)
(defvar *writer-result* nil)

(deftest identity-table-growth
  (let ((table (pocket-compiler::make-eq-table))
        (keys (pocket.sys::%make-simple-vector 300 nil)) (index 0))
    (pocket-compiler::eq-put table nil nil)
    (multiple-value-bind (value present) (pocket-compiler::eq-get table nil)
      (is present) (is (null value)))
    (tagbody insert
      (if (= index 300) (go inserted) nil)
      (let ((key (make-symbol "same-name")))
        (pocket.sys::%set-svref keys index key)
        (pocket-compiler::eq-put table key index))
      (setq index (1+ index))
      (go insert)
     inserted)
    (setq index 0)
    (tagbody lookup
      (if (= index 300) (go looked-up) nil)
      (multiple-value-bind (value present)
          (pocket-compiler::eq-get table (svref keys index))
        (is present) (is (= value index)))
      (setq index (1+ index))
      (go lookup)
     looked-up)
    (multiple-value-bind (value present)
        (pocket-compiler::eq-get table (make-symbol "same-name"))
      (is (null present)) (is (null value)))))

(deftest signed-wire-integers
  (let ((out (pocket-compiler::fasl-output)))
    (pocket-compiler::fasl-u32 out 0)
    (pocket-compiler::fasl-patch-u32 out 0 -129)
    (let ((bytes (pocket-compiler::fasl-finish out)))
      (is (= (pocket.sys::%u8-ref bytes 0) 127))
      (is (= (pocket.sys::%u8-ref bytes 1) 255))
      (is (= (pocket.sys::%u8-ref bytes 2) 255))
      (is (= (pocket.sys::%u8-ref bytes 3) 255)))))

(deftest target-integer-boundaries
  (let* ((bits (pocket.sys::%fixnum-bits))
         (minimum (ash -1 (1- bits))) (maximum (- -1 minimum)))
    (is (pocket-compiler::fasl-target-fixnum-p minimum bits))
    (is (pocket-compiler::fasl-target-fixnum-p maximum bits))
    (is (not (pocket-compiler::fasl-target-fixnum-p 1.5 bits)))
    ;; A 64-bit development host can also check the narrower target without
    ;; first truncating its literals to the target representation.
    (if (> bits 31)
        (progn
          (is (pocket-compiler::fasl-target-fixnum-p -1073741824 31))
          (is (pocket-compiler::fasl-target-fixnum-p 1073741823 31))
          (is (not (pocket-compiler::fasl-target-fixnum-p -1073741825 31)))
          (is (not (pocket-compiler::fasl-target-fixnum-p 1073741824 31))))
        nil)))

(deftest writer-without-source-compiler
  (let* ((minimum (ash -1 (1- (pocket.sys::%fixnum-bits))))
         (maximum (- (1+ minimum)))
         (cycle (cons minimum nil))
         (shared (make-symbol "shared"))
         (literal (vector cycle maximum shared shared 1.5 "λ🙂"))
         (code (pocket-compiler::fasl-output)))
    (rplacd cycle cycle)
    ;; A normal initializer: CONST literal; SET_GLOBAL result; RETURN.
    (pocket-compiler::fasl-byte code 1) (pocket-compiler::fasl-u32 code 0)
    (pocket-compiler::fasl-byte code 3) (pocket-compiler::fasl-u32 code 1)
    (pocket-compiler::fasl-byte code 14)
    (let* ((proto (pocket.sys::%make-proto
                   nil 0 nil 0 0 1 (pocket.sys::%fixnum-bits)
                   (pocket-compiler::fasl-finish code)
                   (vector literal '*writer-result*)))
           (image (pocket-compiler::write-fasl (list proto) 0))
           (entry (pocket.sys::%load-fasl image)))
      (is (null (funcall entry)))
      (is (= (car (svref *writer-result* 0)) minimum))
      (is (eq (cdr (svref *writer-result* 0)) (svref *writer-result* 0)))
      (is (= (svref *writer-result* 1) maximum))
      (is (eq (svref *writer-result* 2) (svref *writer-result* 3)))
      (is (null (symbol-package (svref *writer-result* 2))))
      (is (= (svref *writer-result* 4) 1.5))
      (is-equal "λ🙂" (svref *writer-result* 5)))))
(finish)
