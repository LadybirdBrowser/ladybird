(module
  (import "spectest" "global_i32" (global $imported_i32 i32))

  (global $i32 (mut i32) (i32.const 0))
  (global $i64 (mut i64) (i64.const 0))
  (global $f32 (mut f32) (f32.const 0))
  (global $f64 (mut f64) (f64.const 0))

  (func (export "read_imported") (param i32) (result i32)
    global.get $imported_i32
    local.get 0
    i32.add
  )

  (func (export "i32_roundtrip") (param i32) (result i32)
    local.get 0
    global.set $i32
    global.get $i32
  )

  (func (export "i64_roundtrip") (param i64) (result i64)
    local.get 0
    global.set $i64
    global.get $i64
  )

  (func (export "f32_roundtrip") (param f32) (result f32)
    local.get 0
    global.set $f32
    global.get $f32
  )

  (func (export "f64_roundtrip") (param f64) (result f64)
    local.get 0
    global.set $f64
    global.get $f64
  )

  (func (export "increment_loop") (param i32) (result i32)
    i32.const 0
    global.set $i32
    block $done
      local.get 0
      i32.eqz
      br_if $done
      loop $loop
        global.get $i32
        i32.const 1
        i32.add
        global.set $i32
        local.get 0
        i32.const 1
        i32.sub
        local.tee 0
        br_if $loop
      end
    end
    global.get $i32
  )
)
