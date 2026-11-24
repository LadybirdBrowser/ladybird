(module
  (import "env" "memory" (memory 1 1 shared))
  ;; A lock at address 0 guards the counter at address 8, which is not accessed atomically.
  (func $lock
    (block $locked
      (loop $retry
        (br_if $locked (i32.eqz (i32.atomic.rmw.cmpxchg (i32.const 0) (i32.const 0) (i32.const 1))))
        (drop (memory.atomic.wait32 (i32.const 0) (i32.const 1) (i64.const -1)))
        (br $retry))))
  (func $unlock
    (i32.atomic.store (i32.const 0) (i32.const 0))
    (drop (memory.atomic.notify (i32.const 0) (i32.const 1))))
  (func (export "run") (param $count i32)
    (loop $again
      (call $lock)
      (i32.store (i32.const 8) (i32.add (i32.load (i32.const 8)) (i32.const 1)))
      (call $unlock)
      (br_if $again (local.tee $count (i32.sub (local.get $count) (i32.const 1))))))
  (func (export "counter") (result i32) (i32.load (i32.const 8))))
