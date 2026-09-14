;; Atomic accesses must be exactly naturally aligned; this one claims byte alignment.
(module
  (memory 1)
  (func (export "load") (param i32) (result i32) (i32.atomic.load align=1 (local.get 0)))
)
