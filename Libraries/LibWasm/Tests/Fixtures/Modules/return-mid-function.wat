(module
  ;; Regression test for https://github.com/LadybirdBrowser/ladybird/issues/9614
  ;;
  ;; The 'return' instruction must drop intermediate working values from the value stack, keeping only the top .arity()
  ;; values (the return values). The spec validation rule for 'return' doesn’t require the value stack to be otherwise
  ;; empty when return fires. So a function is allowed to leave extras on the stack, provided the top items match the
  ;; result types.
  ;;
  ;; Before the fix for #9614, the interpreter's return_ handler only shrank the label stack and left residual values on
  ;; the shared value stack, leaking them to the caller.

  ;; Pushes residual 99, then returns 42. The residual must be discarded.
  (func $leaky (result i32)
    i32.const 99
    i32.const 42
    return
  )

  ;; Deterministic correctness check: caller pushes 10, calls $leaky, adds. With the fix: value stack after
  ;; call = [10, 42]; i32.add yields 52. Without the fix: residual 99 sits between 10 and 42. So, i32.add consumes
  ;; 99+42=141. Wrong answer, caught immediately without ASan.
  (func (export "test_add") (result i32)
    i32.const 10
    call $leaky
    i32.add
  )

  ;; Pushes three values, then returns the top one. The bottom two are residuals; 'return' is required to discard them.
  (func $return_with_residuals (result i32)
    i32.const 1111
    i32.const 2222
    i32.const 42
    return
  )

  ;; Calls $return_with_residuals 100 times. Without the fix for #9614, each call leaks two residuals into the driver's
  ;; value stack. Under ASan, that eventually overflows the value stack's inline storage as heap-buffer-overflow. With
  ;; the fix, every iteration is net-zero and acc == 100 * 42.
  (func (export "drive") (result i32)
    (local $i i32)
    (local $acc i32)
    (loop $loop
      call $return_with_residuals
      local.get $acc
      i32.add
      local.set $acc

      local.get $i
      i32.const 1
      i32.add
      local.tee $i
      i32.const 100
      i32.lt_s
      br_if $loop
    )
    local.get $acc
  )
)
