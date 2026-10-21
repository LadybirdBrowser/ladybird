;; Loops of the shapes loop invariant code motion and value numbering are for, as compilers emit them: calls in a
;; loop, which load their callee's FunctionHeader, globals the loop only reads, and repeated address arithmetic.
;; Build with `wasm-tools parse loops.wat -o loops.wasm` and time with examples/run_kernel.rs.
(module
  (memory 1)
  (global $offset (mut i32) (i32.const 3))
  (global $base (mut i32) (i32.const 1024))
  (global $scale (mut i32) (i32.const 7))

  (func $leaf (param i32) (result i32)
    local.get 0
    global.get $offset
    i32.add)

  ;; Function 1: n calls of a leaf in a loop.
  (func (export "calls") (param $n i32) (result i32)
    (local $i i32) (local $sum i32)
    loop $loop
      local.get $sum
      local.get $i
      call $leaf
      i32.add
      local.set $sum
      local.get $i
      i32.const 1
      i32.add
      local.tee $i
      local.get $n
      i32.lt_u
      br_if $loop
    end
    local.get $sum)

  ;; Function 2: a sum over memory at an address and with a factor that globals hold, n times over 256 words.
  (func (export "globals") (param $n i32) (result i32)
    (local $i i32) (local $j i32) (local $sum i32)
    loop $outer
      i32.const 0
      local.set $j
      loop $inner
        local.get $sum
        global.get $base
        local.get $j
        i32.const 2
        i32.shl
        i32.add
        i32.load
        global.get $scale
        i32.mul
        i32.add
        local.set $sum
        local.get $j
        i32.const 1
        i32.add
        local.tee $j
        i32.const 256
        i32.lt_u
        br_if $inner
      end
      local.get $i
      i32.const 1
      i32.add
      local.tee $i
      local.get $n
      i32.lt_u
      br_if $outer
    end
    local.get $sum))
