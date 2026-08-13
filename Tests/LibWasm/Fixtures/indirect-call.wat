(module
  (type $unary (func (param i32) (result i32)))
  (table 1 funcref)
  (elem (i32.const 0) $increment)

  (func $increment (type $unary) (param i32) (result i32)
    local.get 0
    i32.const 1
    i32.add)

  (func (export "run") (param i32) (result i32)
    local.get 0
    i32.const 0
    call_indirect (type $unary)))
