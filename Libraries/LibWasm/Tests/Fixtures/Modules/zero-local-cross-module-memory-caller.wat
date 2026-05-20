(module
  (import "callee" "touch" (func $touch))

  (memory (export "memory") 1)
  (data (i32.const 0) "\2a")

  ;; After calling into the imported wasm function, reads must still use this
  ;; module's default memory, not the callee's memory.
  (func (export "call_and_read") (result i32)
    call $touch
    i32.const 0
    i32.load8_u
  )
)
