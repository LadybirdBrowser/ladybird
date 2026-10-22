;; Arrays of constant expressions with the largest lengths, which do not fit in memory.
(module
  (type $a (array (mut i64)))
  (global (ref $a) (array.new_default $a (i32.const -1))))
