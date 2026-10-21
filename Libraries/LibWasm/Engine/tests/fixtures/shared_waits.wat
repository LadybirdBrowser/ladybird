;; Waits and notifies on a shared memory that no other thread uses.
(module
  (memory 1 1 shared)
  (func (export "wait") (param i32 i32 i64) (result i32)
    (memory.atomic.wait32 (local.get 0) (local.get 1) (local.get 2)))
  (func (export "notify") (param i32) (result i32)
    (memory.atomic.notify (local.get 0) (i32.const 1)))
  (func (export "or") (param i32 i32) (result i32)
    (i32.atomic.rmw.or (local.get 0) (local.get 1)))
)
