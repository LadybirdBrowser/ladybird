(module
  (type $binop (func (param i32 i32) (result i32)))
  (type $unop (func (param i32) (result i32)))
  (type $nullary (func (result i32)))

  (import "./WebAssembly-module-records-provider.wasm" "add" (func $add (type $binop)))
  (import "./WebAssembly-module-records-provider.wasm" "answer" (global $answer i32))
  (import "./WebAssembly-module-records-provider.wasm" "memory" (memory $memory 1))
  (import "./WebAssembly-module-records-functions.js" "double" (func $double (type $unop)))
  (import "./WebAssembly-module-records-functions.js" "localAdd" (func $localAdd (type $binop)))
  (import "./WebAssembly-module-records-functions.js" "seven" (global $seven i32))

  (func $readAnswer (type $nullary)
    global.get $answer)

  (func $callDouble (type $unop)
    local.get 0
    call $double)

  (func $sumOfImports (type $nullary)
    global.get $answer
    global.get $seven
    i32.add)

  (export "add" (func $add))
  (export "double" (func $double))
  (export "localAdd" (func $localAdd))
  (export "readAnswer" (func $readAnswer))
  (export "callDouble" (func $callDouble))
  (export "sumOfImports" (func $sumOfImports))
  (export "memory" (memory $memory)))
