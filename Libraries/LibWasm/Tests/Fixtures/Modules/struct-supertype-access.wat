(module
  (type $small (sub (struct (field (mut i64)) (field (mut i64)) (field (mut i64)) (field (mut i64)))))
  (type $large (sub $small (struct
    (field (mut i64)) (field (mut i64)) (field (mut i64)) (field (mut i64))
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128 v128)
    (field v128 v128 v128 v128 v128))))

  (global $object (mut (ref null $small)) (ref.null $small))
  (global $neighbor (mut (ref null $small)) (ref.null $small))

  (func (export "allocate")
    (local $object (ref null $large))
    (local.set $object (struct.new_default $large))
    (struct.set $large 0 (local.get $object) (i64.const 1))
    (struct.set $large 1 (local.get $object) (i64.const 2))
    (struct.set $large 2 (local.get $object) (i64.const 3))
    (struct.set $large 3 (local.get $object) (i64.const 4))
    (global.set $object (local.get $object))
    (global.set $neighbor (struct.new_default $large)))

  (func (export "get") (param $count i32) (result i64)
    (local $i i32)
    (local $sum i64)
    (loop $again
      (local.set $sum
        (i64.add (local.get $sum)
          (i64.add
            (i64.add (struct.get $small 0 (global.get $object)) (struct.get $small 1 (global.get $object)))
            (i64.add (struct.get $small 2 (global.get $object)) (struct.get $small 3 (global.get $object))))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $count))))
    (local.get $sum))

  (func (export "set") (param $value i64) (param $count i32) (result i64)
    (local $i i32)
    (local $object (ref null $large))
    (local $neighbor (ref null $large))
    (loop $again
      (struct.set $small 0 (global.get $object) (local.get $value))
      (struct.set $small 1 (global.get $object) (i64.add (local.get $value) (i64.const 1)))
      (struct.set $small 2 (global.get $object) (i64.add (local.get $value) (i64.const 2)))
      (struct.set $small 3 (global.get $object) (i64.add (local.get $value) (i64.const 3)))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $count))))
    (local.set $object (ref.cast (ref $large) (global.get $object)))
    (local.set $neighbor (ref.cast (ref $large) (global.get $neighbor)))
    (i64.add
      (i64.add
        (i64.add (struct.get $large 0 (local.get $object)) (struct.get $large 1 (local.get $object)))
        (i64.add (struct.get $large 2 (local.get $object)) (struct.get $large 3 (local.get $object))))
      (i64.add
        (i64.add (struct.get $large 0 (local.get $neighbor)) (struct.get $large 1 (local.get $neighbor)))
        (i64.add (struct.get $large 2 (local.get $neighbor)) (struct.get $large 3 (local.get $neighbor)))))))
