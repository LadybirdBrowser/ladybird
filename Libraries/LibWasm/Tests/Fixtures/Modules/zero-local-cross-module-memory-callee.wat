(module
  (memory (export "memory") 1)
  (data (i32.const 0) "\63")

  ;; Zero params and zero locals: this is the frame shape that previously
  ;; looked non-owning during unwind.
  (func (export "touch")
    i32.const 0
    i32.const 99
    i32.store8
  )
)
