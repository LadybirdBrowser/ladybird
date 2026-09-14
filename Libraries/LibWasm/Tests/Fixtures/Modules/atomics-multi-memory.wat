;; Atomic accesses to a memory other than the first one.
(module
  (memory 1)
  (memory 1)
  (func (export "store") (param i32 i32) (i32.atomic.store 1 (local.get 0) (local.get 1)))
  (func (export "load") (param i32) (result i32) (i32.atomic.load 1 (local.get 0)))
  (func (export "load_first") (param i32) (result i32) (i32.load 0 (local.get 0)))
)
