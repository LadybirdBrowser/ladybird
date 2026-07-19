(module
  (import "callee" "touch" (func $touch))
  (global $value (mut i32) (i32.const 41))

  (func (export "call_and_read") (result i32)
    i32.const 42
    global.set $value
    call $touch
    global.get $value
  )
)
