;; A shared memory, exported, and waits on it.
(module
  (memory (export "memory") 1 1 shared)
  (func (export "wait") (param i32 i32 i64) (result i32)
    (memory.atomic.wait32 (local.get 0) (local.get 1) (local.get 2))))
