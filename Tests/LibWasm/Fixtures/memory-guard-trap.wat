(module
  (memory (export "memory") 1)

  ;; Plain load/store to the default memory. Compiled accesses are unchecked and rely on the
  ;; memory's guarded reservation faulting past the committed size.
  (func (export "load") (param i32) (result i32)
    (i32.load (local.get 0)))
  (func (export "store") (param i32) (param i32)
    (i32.store (local.get 0) (local.get 1)))

  ;; A large memarg offset pushes the effective address (base + offset) high into the guarded
  ;; reservation, exercising the near-maximum-u32-plus-offset end of the span.
  (func (export "load_high") (param i32) (result i32)
    (i32.load offset=0x40000000 (local.get 0))))
