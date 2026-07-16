(module
  (import "test" "label_stack_size" (func $label_stack_size (result i32)))

  ;; A reference local keeps this function in the direct-threaded interpreter
  ;; when its caller has been compiled by Cranelift.
  (func $callee (local externref)
    (if (i32.const 1)
      (then (br 0))
      (else)))

  (func (export "run") (result i32)
    (call $callee)
    (call $label_stack_size)))
