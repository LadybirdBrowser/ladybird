;; Functions that take values from the host as arguments and results.
(module
  (import "env" "reference" (func $reference (result anyref)))
  (func (export "is_null") (param externref) (result i32)
    (ref.is_null (local.get 0)))
  (func (export "take_non_null") (param (ref extern)) (result i32)
    (i32.const 1))
  (func (export "host_is_null") (result i32)
    (ref.is_null (call $reference))))
