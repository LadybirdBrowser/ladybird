;; A loop whose only call is a `return_call_ref` of a null reference, on a path that the loop takes only if `$call`
;; is nonzero. Everything the tail call needs must stay behind its null check, even though the loop never calls a
;; function and returns to its header: reading the null reference's header anywhere else would fault.
(module
  (type $t (func (result i32)))
  (global $g (mut (ref null $t)) (ref.null $t))
  (func (export "run") (param $call i32) (param $count i32) (result i32)
    (local $r (ref null $t))
    (local $i i32)
    (local.set $r (global.get $g))
    (loop $l
      (if (local.get $call) (then (return_call_ref $t (local.get $r))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $l (i32.lt_u (local.get $i) (local.get $count))))
    (i32.const 7)))
