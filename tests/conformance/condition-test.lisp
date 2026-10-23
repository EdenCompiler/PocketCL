;;; The M3 subset deliberately uses no restart or condition-definition APIs.
;;; These cases have the same expected results on PocketCL and SBCL.
(in-package :pocket.test)
(defvar *condition-phase* nil)

(deftest handlers-and-unwind
  (is (= 42 (handler-case (error "caught") (error () 42))))
  (is-equal '(2 3) (multiple-value-list
                    (handler-case (values 2 3) (error () nil))))
  (is-equal nil (multiple-value-list
                (handler-case (values) (error () nil))))
  (is (= 7 (handler-case (values 3 4) (:no-error (a b) (+ a b)))))
  (is-equal '(5 nil) (handler-case (values 5)
                       (:no-error (a &optional b) (list a b))))
  (let ((*condition-phase* :body))
    (is (eq :cleaned
            (handler-case
                (unwind-protect (error "unwind") (setq *condition-phase* :cleaned))
              (error () *condition-phase*))))
    (is (eq *condition-phase* :cleaned)))
  (let ((*condition-phase* :outside))
    (is-equal '(:inside :after)
      (list
       (catch 'handled
         (handler-bind ((error #'(lambda (condition)
                                  (declare (ignore condition))
                                  (throw 'handled *condition-phase*))))
           (let ((*condition-phase* :inside))
             (unwind-protect (error "before unwind")
               (setq *condition-phase* :cleaned)))))
       (progn (setq *condition-phase* :after) *condition-phase*)))
    (is (eq *condition-phase* :after)))
  (let ((seen nil))
    (handler-bind ((error #'(lambda (condition) (declare (ignore condition))
                             (setq seen (cons :first seen))))
                   (error #'(lambda (condition) (declare (ignore condition))
                             (setq seen (cons :second seen)))))
      (signal 'error))
    (is-equal '(:second :first) seen))
  (let ((seen nil))
    ;; A nested signal cannot re-enter either handler in the current cluster.
    (handler-bind ((error #'(lambda (condition) (declare (ignore condition))
                             (setq seen (cons :outer seen)))))
      (handler-bind ((error #'(lambda (condition) (declare (ignore condition))
                               (setq seen (cons :inner seen))
                               (signal 'error))))
        (signal 'error)))
    (is-equal '(:outer :outer :inner) seen))
  (is (null (signal "declined")))
  (is (= 9 (handler-case (error "first match")
             (error () 9) (t () 10))))
  (is (= 11 (handler-case (handler-case (error "nested") (error () (error "outer")))
              (error () 11)))))
(finish)
