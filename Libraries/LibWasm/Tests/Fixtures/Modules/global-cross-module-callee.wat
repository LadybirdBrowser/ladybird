(module
  (global $value (mut i32) (i32.const 1))

  (func (export "touch")
    i32.const 99
    global.set $value
  )

  (func (export "read") (result i32)
    global.get $value
  )
)
