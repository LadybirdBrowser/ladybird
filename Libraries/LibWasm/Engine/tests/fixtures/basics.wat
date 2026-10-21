(module
  (memory (export "memory") 1)
  (global $counter (mut i32) (i32.const 10))
  (func (export "add") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)
  (func $fib (export "fib") (param i64) (result i64)
    local.get 0
    i64.const 2
    i64.lt_u
    if (result i64)
      local.get 0
    else
      local.get 0
      i64.const 1
      i64.sub
      call $fib
      local.get 0
      i64.const 2
      i64.sub
      call $fib
      i64.add
    end)
  (func (export "sum_to") (param i32) (result i32)
    (local i32 i32)
    block
      loop
        local.get 1
        local.get 0
        i32.ge_s
        br_if 1
        local.get 1
        i32.const 1
        i32.add
        local.tee 1
        local.get 2
        i32.add
        local.set 2
        br 0
      end
    end
    local.get 2)
  (func (export "store_and_load") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.store offset=4
    local.get 0
    i32.load8_u offset=5)
  (func (export "classify") (param i32) (result i32)
    block
      block
        block
          local.get 0
          br_table 0 1 2
        end
        i32.const 100
        return
      end
      i32.const 200
      return
    end
    i32.const 300)
  (func (export "bump") (result i32)
    global.get $counter
    i32.const 5
    i32.add
    global.set $counter
    global.get $counter)
  (func (export "trap") (result i32)
    unreachable)
  (func (export "out_of_bounds") (result i32)
    i32.const 65536
    i32.load)
  (func $recurse (export "recurse") (param i32) (result i32)
    local.get 0
    i32.const 1
    i32.add
    call $recurse)
  (func (export "multi") (result i32 i64 i32)
    i32.const 1
    i64.const 2
    i32.const 3)
  (func (export "select") (param i32) (result i32)
    i32.const 11
    i32.const 22
    local.get 0
    select)
  (func (export "drop_in_block") (result i32)
    block (result i32)
      i32.const 1
      i32.const 2
      i32.const 3
      br 0
    end))
