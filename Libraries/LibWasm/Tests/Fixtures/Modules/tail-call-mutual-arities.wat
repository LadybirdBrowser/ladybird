(module
  ;; Mutual tail calls between functions whose stack arguments differ in number, which must run in constant stack
  ;; space: $few takes no stack arguments, $many takes several (more than the six integer argument registers).
  (func $few (export "few") (param $n i32) (result i32)
    (if (result i32) (i32.eqz (local.get $n))
      (then (i32.const 0))
      (else (return_call $many (i32.sub (local.get $n) (i32.const 1))
        (i32.const 1) (i32.const 2) (i32.const 3) (i32.const 4) (i32.const 5) (i32.const 6) (i32.const 7)
        (i32.const 8) (i32.const 9)))))
  (func $many (param $n i32) (param i32 i32 i32 i32 i32 i32 i32 i32 i32) (result i32)
    (return_call $odd (local.get $n) (local.get 1) (local.get 2)))
  ;; An odd number of stack slots in between, for the alignment of the frame.
  (func $odd (param $n i32) (param i32 i32) (result i32)
    (return_call $seven (local.get $n) (i32.const 1) (i32.const 2) (i32.const 3) (i32.const 4) (i32.const 5)
      (i32.const 6) (i32.const 7)))
  (func $seven (param $n i32) (param i32 i32 i32 i32 i32 i32 i32) (result i32)
    (return_call $few (local.get $n)))
  ;; The same with a result area: more results than result registers.
  (func $few_results (export "few_results") (param $n i32) (result i32 i32 i32)
    (if (result i32 i32 i32) (i32.eqz (local.get $n))
      (then (i32.const 1) (i32.const 2) (i32.const 3))
      (else (return_call $many_results (i32.sub (local.get $n) (i32.const 1))
        (i32.const 1) (i32.const 2) (i32.const 3) (i32.const 4) (i32.const 5) (i32.const 6) (i32.const 7)
        (i32.const 8)))))
  (func $many_results (param $n i32) (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32 i32 i32)
    (return_call $few_results (local.get $n))))
