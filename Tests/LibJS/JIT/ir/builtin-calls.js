// Calls of builtins with inline paths for common arguments, and a cold slow path for the rest: String.fromCharCode of
// ASCII code units, the String, Array, Object and Boolean constructors called as functions (which convert numbers and
// other primitives through the runtime), and CreateDataPropertyOrThrow (as Array.prototype.map uses it) of elements of
// packed and holey arrays.

// OPTIONS: inline-max-size=400,inline-budget=800

function convert(code, value) {
    return String.fromCharCode(code) + String(value) + Boolean(value) + typeof Object(value) + Array(code).length;
}
jit.prepare(convert);
for (let i = 0; i < 10; ++i) {
    convert(65 + i, "x");
    convert(97, i);
}
jit.compile(convert);

// CHECK-FUNCTION: convert
// CHECK-PASS: dead code elimination
// CHECK: [[CODE:v[0-9]+]]:int32 = UnboxInt32
// CHECK-NEXT: [[UNIT:v[0-9]+]]:int32 = Int32BitwiseAnd ([[CODE]], #i32:65535)
// CHECK-NEXT: BranchInt32LessThan ([[UNIT]], #i32:128)
// CHECK: SingleCharacterString
// CHECK: CallSlowPath Call @
// CHECK: BranchString
// CHECK: IntegerToString
// CHECK: PrimitiveToString
// CHECK: CallSlowPath Call
// CHECK: BranchTruthy
// CHECK: BranchObject
// CHECK: BranchNullish
// CHECK: ToObject
// CHECK: CallSlowPath Call
// CHECK: [[LENGTH:v[0-9]+]]:int32 = UnboxInt32
// CHECK-NEXT: ArrayCreate ([[LENGTH]], {{#[0-9a-fx]+}})

function double(x) {
    return x * 2;
}
function doubleAll(array) {
    return array.map(double);
}
jit.prepare(doubleAll);
for (let i = 0; i < 10; ++i) doubleAll([1, 2, 3]);
jit.compile(doubleAll);

// CHECK-FUNCTION: doubleAll
// CHECK-PASS: dead code elimination
// CHECK: BranchElementsKind Packed
// CHECK: StoreElementAt Packed
// CHECK: BranchExtensible
// CHECK: StoreElementAt Holey
// CHECK: CallSlowPath CreateDataPropertyOrThrow
