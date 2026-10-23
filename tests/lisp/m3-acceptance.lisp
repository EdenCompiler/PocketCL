(in-package :pocket.test)

(defvar *m3-overflow-special* :root)

(defun m3-grandparent-reader ()
  (let ((value 314))
    (let ((middle (lambda () (lambda () value))))
      (funcall (funcall middle)))))

(defun m3-mutable-capture ()
  (let ((value 1))
    (let ((read-value (lambda () value))
          (write-value (lambda (new-value) (setq value new-value))))
      (funcall write-value 2718)
      (funcall read-value))))

(defun m3-tail-counter (remaining total)
  (if (= remaining 0)
      (list remaining total)
      (m3-tail-counter (1- remaining) (1+ total))))

;; The recursive call is deliberately beneath + so the compiler cannot turn
;; it into a tail call. M3's overflow condition must escape while this stack
;; and its dynamic bindings are still live.
(defun m3-nontail-recursion (remaining)
  (if (= remaining 0)
      0
      (+ 1 (m3-nontail-recursion (1- remaining)))))

(defun m3-hot-add (left right)
  (+ left right))

(deftest m3-captures-and-million-tail-calls
  (is (= (m3-grandparent-reader) 314))
  (is (= (m3-mutable-capture) 2718))
  ;; Both returned values expose the exact completed iteration count while
  ;; staying in fixnum range on the 31-bit and 62-bit targets.
  (is-equal '(0 1000017) (m3-tail-counter 1000000 17)))

(deftest m3-overflow-handlers-run-before-unwind
  (let ((cleanup :pending)
        (handler-snapshot nil)
        (caught nil))
    (let ((*m3-overflow-special* :outer))
      (setq caught
            (handler-case
                (handler-bind
                    ((pocket:stack-overflow
                      (lambda (condition)
                        (declare (ignore condition))
                        (setq handler-snapshot
                              (list *m3-overflow-special* cleanup))
                        ;; The error-hook path must ignore a returned packet
                        ;; and still transfer the original overflow condition.
                        (values :hook-first :hook-second))))
                  (let ((*m3-overflow-special* :inner))
                    (unwind-protect
                         (m3-nontail-recursion 1000000)
                      (setq cleanup :cleaned))))
              (pocket:stack-overflow ()
                (list :caught *m3-overflow-special* cleanup))))
      (is-equal '(:inner :pending) handler-snapshot)
      (is-equal '(:caught :outer :cleaned) caught)
      (is (eq *m3-overflow-special* :outer)))
    (is (eq cleanup :cleaned))))

(deftest m3-overflow-preserves-zero-values-and-usability
  ;; A hook returning zero values must also leave the error transfer intact;
  ;; the following arithmetic proves the VM remains usable in this file.
  (is (null
       (multiple-value-list
        (handler-case
            (handler-bind
                ((pocket:stack-overflow
                  (lambda (condition)
                    (declare (ignore condition))
                    (values))))
              (m3-nontail-recursion 1000000))
          (pocket:stack-overflow () (values))))))
  (is (= (+ 20 22) 42)))

(deftest m3-hot-operator-redefinition
  ;; The compiler may emit its integer ADD fast path only while the original
  ;; CL:+ function is installed. Restore the global cell if the replacement
  ;; call signals an error; assertions run only after restoration.
  (let ((original-plus (symbol-function '+))
        (replacement-result nil))
    (unwind-protect
         (progn
           (pocket.sys::%set-symbol-function
            '+ (lambda (left right)
                 (declare (ignore left right))
                 87))
           (setq replacement-result (m3-hot-add 10 20)))
      (pocket.sys::%set-symbol-function '+ original-plus))
    (is (= replacement-result 87))
    (is (eq (symbol-function '+) original-plus))))

(finish)
