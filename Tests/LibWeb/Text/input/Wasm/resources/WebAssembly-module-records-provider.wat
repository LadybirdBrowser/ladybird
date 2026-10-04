(module
  (type $binop (func (param i32 i32) (result i32)))

  (func $add (type $binop)
    local.get 0
    local.get 1
    i32.add)

  (memory $memory 1)
  (global $answer i32 (i32.const 42))

  (export "add" (func $add))
  (export "answer" (global $answer))
  (export "memory" (memory $memory)))
