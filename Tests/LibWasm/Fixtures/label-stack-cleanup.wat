(module
  (import "test" "label_stack_size" (func $label_stack_size (result i32)))

  ;; Branches out of an if; the caller then asks the host how deep its label
  ;; stack is.
  (func $callee (local externref)
    (if (i32.const 1)
      (then (br 0))
      (else)))

  (func (export "run") (result i32)
    (call $callee)
    (call $label_stack_size)))
