;; An active element segment, which instantiation copies into the table and drops, and a passive one.
(module
  (table 4 funcref)
  (func $f (result i32) (i32.const 7))
  (elem (i32.const 1) func $f)
  (elem $passive func $f $f)
  (func (export "call") (param i32) (result i32)
    (call_indirect (result i32) (local.get 0)))
  (func (export "init") (param i32)
    (table.init $passive (local.get 0) (i32.const 0) (i32.const 2))))
