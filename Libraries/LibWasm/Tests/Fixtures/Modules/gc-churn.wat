;; Allocates objects, large arrays, and exceptions that become garbage right away.
(module
  (type $node (struct (field (mut i64)) (field (ref null $node))))
  (type $bytes (array (mut i32)))
  (tag $thrown (param (ref null $node)))
  (func (export "churn") (param $count i32) (result i64)
    (local $index i32)
    (local $last (ref null $node))
    (loop $again
      (local.set $last (struct.new $node (i64.extend_i32_u (local.get $index)) (ref.null $node)))
      ;; 64 KiB of elements, which live outside the heap.
      (drop (array.new_default $bytes (i32.const 16384)))
      (block $caught (result (ref null $node))
        (try_table (catch $thrown $caught)
          (throw $thrown (local.get $last)))
        (unreachable))
      (drop)
      (local.set $index (i32.add (local.get $index) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $index) (local.get $count))))
    (struct.get $node 0 (local.get $last)))
)
