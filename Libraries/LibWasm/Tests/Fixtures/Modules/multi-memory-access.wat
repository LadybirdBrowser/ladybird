(module
  (memory $memory0 1)
  (memory $memory1 1 3)

  (func (export "roundtrip") (param i32) (result i32)
    i32.const 32
    local.get 0
    i32.store $memory1
    i32.const 32
    i32.load $memory1
  )

  (func (export "load_at") (param i32) (result i32)
    local.get 0
    i32.load $memory1
  )

  (func (export "store_at") (param i32) (result i32)
    local.get 0
    i32.const 42
    i32.store $memory1
    i32.const 1
  )

  (func (export "memory_one_is_distinct") (result i32)
    i32.const 48
    i32.const 42
    i32.store $memory1
    i32.const 48
    i32.load $memory0
  )

  (func $grow_memory_one
    i32.const 1
    memory.grow $memory1
    drop
  )

  (func (export "grow_and_roundtrip") (param i32) (result i32)
    i32.const 1
    memory.grow $memory1
    drop
    i32.const 65536
    local.get 0
    i32.store $memory1
    i32.const 65536
    i32.load $memory1
  )

  (func (export "call_grow_and_roundtrip") (param i32) (result i32)
    call $grow_memory_one
    i32.const 131072
    local.get 0
    i32.store $memory1
    i32.const 131072
    i32.load $memory1
  )
)
