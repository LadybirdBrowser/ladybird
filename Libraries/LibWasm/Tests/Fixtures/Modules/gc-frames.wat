;; References that live in frames while the collector runs: in locals and on the operand stack across calls that
;; allocate enough to collect, in inline and out-of-line objects, and as i31 values. Every function
;; returns a checksum that is only right if nothing they still refer to was freed or overwritten.
(module
  (type $node (struct (field $value (mut i64)) (field $next (mut (ref null $node)))))
  (type $nodes (array (mut (ref null $node))))
  (type $bytes (array (mut i8)))
  (type $vector (struct (field v128) (field (ref null $node)) (field f64)))
  ;; 600 fields of 8 bytes: data outside the heap.
  (type $wide (array (mut i64)))

  ;; Allocates 32 KB of garbage per count, written so that it takes memory, so that collections happen during the
  ;; calls below.
  (func $churn (param $count i32)
    (local $i i32)
    (loop $again
      (drop (array.new $bytes (i32.const 1) (i32.const 4096)))
      (drop (struct.new $node (i64.const -1) (ref.null $node)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $count)))))

  ;; A list of $length nodes held only by a local, with garbage allocated between every two nodes.
  (func (export "list") (param $length i32) (result i64)
    (local $head (ref null $node))
    (local $i i32)
    (local $sum i64)
    (loop $build
      (local.set $head (struct.new $node (i64.extend_i32_u (local.get $i)) (local.get $head)))
      (call $churn (i32.const 64))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $build (i32.lt_u (local.get $i) (local.get $length))))
    (block $done
      (loop $walk
        (br_if $done (ref.is_null (local.get $head)))
        (local.set $sum (i64.add (local.get $sum) (struct.get $node $value (local.get $head))))
        (local.set $head (struct.get $node $next (local.get $head)))
        (br $walk)))
    (local.get $sum))

  ;; References on the operand stack across calls: struct.new's operands are evaluated with calls in between.
  (func $make (param $value i64) (result (ref $node))
    (call $churn (i32.const 32))
    (struct.new $node (local.get $value) (ref.null $node)))

  (func (export "operands") (param $rounds i32) (result i64)
    (local $i i32)
    (local $sum i64)
    (local $pair (ref null $node))
    (loop $again
      (local.set $pair
        (struct.new $node
          (i64.const 0)
          (struct.new $node
            (struct.get $node $value (call $make (i64.extend_i32_u (local.get $i))))
            (call $make (i64.const 1000)))))
      (local.set $sum
        (i64.add (local.get $sum)
          (i64.add
            (struct.get $node $value (struct.get $node $next (local.get $pair)))
            (struct.get $node $value
              (struct.get $node $next (struct.get $node $next (local.get $pair)))))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $rounds))))
    (local.get $sum))

  ;; An array of references with its data outside the heap, filled across collections.
  (func (export "array") (param $length i32) (result i64)
    (local $array (ref null $nodes))
    (local $i i32)
    (local $sum i64)
    (local.set $array (array.new_default $nodes (local.get $length)))
    (loop $fill
      (array.set $nodes (local.get $array) (local.get $i)
        (struct.new $node (i64.extend_i32_u (local.get $i)) (ref.null $node)))
      (call $churn (i32.const 16))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $fill (i32.lt_u (local.get $i) (local.get $length))))
    (local.set $i (i32.const 0))
    (loop $walk
      (local.set $sum
        (i64.add (local.get $sum)
          (struct.get $node $value (array.get $nodes (local.get $array) (local.get $i)))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $walk (i32.lt_u (local.get $i) (local.get $length))))
    (local.get $sum))

  ;; A large array of i64s (data outside the heap), written before and read after collections.
  (func (export "wide") (param $rounds i32) (result i64)
    (local $array (ref null $wide))
    (local $i i32)
    (local $sum i64)
    (local.set $array (array.new $wide (i64.const 7) (i32.const 600)))
    (loop $again
      (array.set $wide (local.get $array) (i32.rem_u (local.get $i) (i32.const 600))
        (i64.extend_i32_u (local.get $i)))
      (call $churn (i32.const 16))
      (local.set $sum
        (i64.add (local.get $sum)
          (array.get $wide (local.get $array) (i32.rem_u (local.get $i) (i32.const 600)))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $rounds))))
    (i64.add (local.get $sum) (array.get $wide (local.get $array) (i32.const 599))))

  ;; Structs with v128 and f64 fields, and casts of what survived.
  (func (export "mixed") (param $rounds i32) (result i64)
    (local $i i32)
    (local $sum i64)
    (local $keep (ref null $vector))
    (local $any anyref)
    (loop $again
      (local.set $keep
        (struct.new $vector
          (i64x2.splat (i64.extend_i32_u (local.get $i)))
          (struct.new $node (i64.const 3) (ref.null $node))
          (f64.convert_i32_u (local.get $i))))
      (local.set $any (ref.i31 (local.get $i)))
      (call $churn (i32.const 16))
      (local.set $sum
        (i64.add (local.get $sum)
          (i64.add
            (i64x2.extract_lane 1 (struct.get $vector 0 (local.get $keep)))
            (i64.add
              (struct.get $node $value (struct.get $vector 1 (local.get $keep)))
              (i64.add
                (i64.trunc_f64_u (struct.get $vector 2 (local.get $keep)))
                (i64.extend_i32_u (i31.get_u (ref.cast (ref i31) (local.get $any)))))))))
      (local.set $any (local.get $keep))
      (call $churn (i32.const 4))
      (local.set $sum
        (i64.add (local.get $sum)
          (struct.get $node $value (struct.get $vector 1 (ref.cast (ref $vector) (local.get $any))))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $rounds))))
    (local.get $sum)))
