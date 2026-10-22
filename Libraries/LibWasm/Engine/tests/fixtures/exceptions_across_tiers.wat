;; A handler in one function for an exception that a function it calls throws after it called the host.
(module
  (import "env" "host" (func $host))
  (tag $tag (param i32))
  (func $throws (export "throws")
    (call $host)
    (throw $tag (i32.const 42)))
  (func (export "catches") (result i32)
    (block $caught (result i32)
      (try_table (catch $tag $caught)
        (call $throws))
      (i32.const 0))))
