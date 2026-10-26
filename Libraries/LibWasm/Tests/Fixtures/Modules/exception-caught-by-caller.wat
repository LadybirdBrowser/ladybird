(module
  (type $values (func (result
    i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32)))
  (tag $thrown (param
    i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32 i32))

  (func $values (type $values)
    (i32.const 0) (i32.const 1) (i32.const 2) (i32.const 3) (i32.const 4) (i32.const 5) (i32.const 6)
    (i32.const 7) (i32.const 8) (i32.const 9) (i32.const 10) (i32.const 11) (i32.const 12) (i32.const 13)
    (i32.const 14) (i32.const 15))

  (func $throw_values
    (throw $thrown (call $values)))

  (func (export "catch_from_callee") (type $values)
    (block $caught (type $values)
      (try_table (catch $thrown $caught)
        (call $throw_values))
      (unreachable)))

  (func $throw_values_compiled
    (throw $thrown (call $values)))

  (func (export "compile_thrower") (param $count i32)
    (loop $again
      (block $caught
        (try_table (catch_all $caught)
          (call $throw_values_compiled)))
      (br_if $again (local.tee $count (i32.sub (local.get $count) (i32.const 1))))))

  (func $call_compiled_thrower
    (call $throw_values_compiled))

  (func (export "catch_from_compiled_callee") (type $values)
    (block $caught (type $values)
      (try_table (catch $thrown $caught)
        (call $call_compiled_thrower))
      (unreachable)))

  (func $loop_then_throw_values
    (local $count i32)
    (local.set $count (i32.const 1000000))
    (loop $again
      (br_if $again (local.tee $count (i32.sub (local.get $count) (i32.const 1)))))
    (throw $thrown (call $values)))

  (func (export "catch_from_callee_continued_compiled") (type $values)
    (block $caught (type $values)
      (try_table (catch $thrown $caught)
        (call $loop_then_throw_values))
      (unreachable))))
