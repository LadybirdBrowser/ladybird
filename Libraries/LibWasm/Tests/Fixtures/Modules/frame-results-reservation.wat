(module
  (type $values (func (result
    i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32
    i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32
    i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)))
  (tag $thrown (param
    i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32
    i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32
    i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))
  (global $exception (mut exnref) (ref.null exn))

  (func $values (export "values") (type $values)
    (i32.const 0) (i32.const 1) (i32.const 2) (i32.const 3) (i32.const 4) (i32.const 5) (i32.const 6)
    (i32.const 7) (i32.const 8) (i32.const 9) (i32.const 10) (i32.const 11) (i32.const 12) (i32.const 13)
    (i32.const 14) (i32.const 15) (i32.const 16) (i32.const 17) (i32.const 18) (i32.const 19) (i32.const 20)
    (i32.const 21) (i32.const 22) (i32.const 23) (i32.const 24) (i32.const 25) (i32.const 26) (i32.const 27)
    (i32.const 28) (i32.const 29) (i32.const 30) (i32.const 31) (i32.const 32) (i32.const 33) (i32.const 34)
    (i32.const 35) (i32.const 36) (i32.const 37) (i32.const 38) (i32.const 39) (i32.const 40) (i32.const 41)
    (i32.const 42) (i32.const 43) (i32.const 44) (i32.const 45) (i32.const 46) (i32.const 47) (i32.const 48)
    (i32.const 49) (i32.const 50) (i32.const 51) (i32.const 52) (i32.const 53) (i32.const 54) (i32.const 55)
    (i32.const 56) (i32.const 57) (i32.const 58) (i32.const 59) (i32.const 60) (i32.const 61) (i32.const 62)
    (i32.const 63))

  (func $store_exception
    (block $caught (result exnref)
      (try_table (catch_all_ref $caught)
        (throw $thrown (call $values)))
      (unreachable))
    (global.set $exception))
  (start $store_exception)

  (func (export "call_values") (param $count i32)
    (loop $again
      (block (call $values) (br 0))
      (br_if $again (local.tee $count (i32.sub (local.get $count) (i32.const 1))))))

  (func (export "catch_values") (type $values)
    (try_table (catch $thrown 0)
      (throw_ref (global.get $exception)))
    (unreachable))

  (func (export "loop_then_tail_call_values") (type $values)
    (local $count i32)
    (local.set $count (i32.const 100000))
    (loop $again
      (br_if $again (local.tee $count (i32.sub (local.get $count) (i32.const 1)))))
    (return_call $values)))
