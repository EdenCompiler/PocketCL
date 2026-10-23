;;; The compiler is an ordinary Lisp program. This file is read after boot,
;;; before the remaining compiler sources, in both bootstrap stages.
(defpackage :pocket-compiler (:use :cl))
(in-package :pocket-compiler)

;;; These small wrappers keep storage policy in Lisp while the kernel only
;;; provides bounds-checked vector/byte access. No generalized places are used
;;; here: SETF and the larger data library belong to later milestones.
(defun vset (vector index value)
  (pocket.sys::%set-svref vector index value))
(defun make-buffer ()
  (vector 0 (pocket.sys::%make-simple-vector 16 nil)))
(defun buffer-length (buffer) (svref buffer 0))
(defun buffer-ref (buffer index) (svref (svref buffer 1) index))
(defun buffer-push (buffer value)
  (let* ((count (buffer-length buffer)) (items (svref buffer 1)))
    (if (= count (length items))
        (let ((larger (pocket.sys::%make-simple-vector (* 2 (length items)) nil))
              (index 0))
          (tagbody copy
            (if (= index count) (go copied) nil)
            (vset larger index (svref items index))
            (setq index (1+ index))
            (go copy)
           copied)
          (vset buffer 1 larger)
          (setq items larger)) nil)
    (vset items count value)
    (vset buffer 0 (1+ count))
    count))
(defun buffer-vector (buffer)
  (let* ((count (buffer-length buffer))
         (result (pocket.sys::%make-simple-vector count nil)) (index 0))
    (tagbody copy
      (if (= index count) (go done) nil)
      (vset result index (buffer-ref buffer index))
      (setq index (1+ index))
      (go copy)
     done)
    result))
