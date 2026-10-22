;; array.new in a constant expression with the largest length, which does not fit in memory.
(module
  (type $a (array (mut i64)))
  (global (ref $a) (array.new $a (i64.const 7) (i32.const -1))))
