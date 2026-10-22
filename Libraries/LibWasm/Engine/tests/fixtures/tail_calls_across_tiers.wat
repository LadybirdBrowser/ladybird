;; Two functions that tail call each other through references, once for each step down to 0.
(module
  (type $step (func (param i64) (result i64)))
  (global $even (ref $step) (ref.func $even))
  (global $odd (ref $step) (ref.func $odd))
  (elem declare func $even $odd)
  (func $even (export "even") (type $step)
    (if (result i64) (i64.eqz (local.get 0))
      (then (i64.const 44))
      (else (return_call_ref $step (i64.sub (local.get 0) (i64.const 1)) (global.get $odd)))))
  (func $odd (export "odd") (type $step)
    (if (result i64) (i64.eqz (local.get 0))
      (then (i64.const 99))
      (else (return_call_ref $step (i64.sub (local.get 0) (i64.const 1)) (global.get $even))))))
