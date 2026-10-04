(module
  (import "./WebAssembly-module-records-cycle-a.wasm" "f" (func $f))
  (func $g)
  (export "g" (func $g)))
