(module
  (type $empty (func))

  (import "./WebAssembly-module-host-function-metadata-imports.js" "globalValue" (global i32))
  (import "./WebAssembly-module-host-function-metadata-imports.js" "function0" (func $function0 (type $empty)))
  (import "./WebAssembly-module-host-function-metadata-imports.js" "function1" (func $function1 (type $empty)))

  (export "function0" (func $function0))
  (export "function1" (func $function1)))
