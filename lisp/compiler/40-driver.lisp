;;; Public compiler entry points. Top-level policy lives here, leaving the C
;;; image driver responsible only for reading forms and recording protos.
(in-package :pocket-compiler)

(defun execute-proto (proto)
  (let ((closure (pocket.sys::%make-closure proto (vector))))
    (funcall closure)))

(defun compile-and-run-forms (forms name)
  (declare (ignore name))
  (let* ((source (cons 'progn forms))
         (proto (compile-form source)))
    (values proto (execute-proto proto))))

(defun top-level-situation-p (situations situation)
  (member situation situations))

(defun process-eval-when (form)
  (let* ((situations (cadr form))
         (body (cddr form))
         (compile-time (or (top-level-situation-p situations :compile-toplevel)
                           (top-level-situation-p situations :execute)))
         (load-time (top-level-situation-p situations :load-toplevel))
         (proto nil) (result nil))
    ;; This bootstrap model compiles every requested top-level body once and
    ;; executes it once. A LOAD request retains that same proto for the image.
    (if (or compile-time load-time)
        (multiple-value-bind (compiled value) (compile-and-run-forms body nil)
          (if load-time (setq proto compiled) nil)
          (setq result value)) nil)
    (values (if load-time proto nil) result)))

(defun process-toplevel (form)
  (if (and (consp form) (eq (car form) 'eval-when))
      (process-eval-when form)
      (multiple-value-bind (proto result)
          (compile-and-run-forms (list form) nil)
        (values proto result))))
