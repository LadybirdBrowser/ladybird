;; The first handler of a legacy try throws a tag that a later catch of the same try tests.
(module
  (tag $first (param i32))
  (tag $second (param i32))
  (func (export "main") (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do (throw $first (i32.const 1)))
          (catch $first
            (throw $second (i32.add (i32.const 41))))
          (catch $second)))
      (catch $second))))
