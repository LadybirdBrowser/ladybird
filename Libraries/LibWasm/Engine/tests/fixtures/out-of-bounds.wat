(module
  (memory 1)
  (func (export "load") (param i32) (result i32)
    (i32.load (local.get 0)))
  (func (export "store") (param i32)
    (i32.store offset=65536 (local.get 0) (i32.const 1))))
