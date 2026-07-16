(module
  (type $result-i32 (func (result i32)))

  (func $target (type $result-i32)
    (i32.const 7))

  (func $tail-caller (type $result-i32)
    (return_call $target))

  (func (export "run") (type $result-i32)
    (i32.add
      (call $tail-caller)
      (i32.const 1))))
