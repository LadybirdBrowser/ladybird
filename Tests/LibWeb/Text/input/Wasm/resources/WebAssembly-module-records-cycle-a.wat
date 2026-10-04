(module
  (import "./WebAssembly-module-records-cycle-b.wasm" "g" (func $g))
  (func $f)
  (export "f" (func $f)))
