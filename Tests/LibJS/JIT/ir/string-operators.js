// REQUIRES: inline-allocation
// Adding strings concatenates them, and an integer the VM has the string of to a string concatenates its string.
// Strict equality of strings compares them, and loose equality with null tests for undefined and null. What the
// inline nodes cannot do they leave to the slow path, with the empty value.

function text(a, b) {
    return a + b === "x1" || a == null;
}
jit.prepare(text);
for (let i = 0; i < 10; ++i) {
    text("x", i);
    text("y", "z");
}
jit.compile(text);

// CHECK-FUNCTION: text
// CHECK-PASS: dead code elimination
// CHECK: BranchString ([[A:v[0-9]+]])
// CHECK: [[INTEGER:v[0-9]+]]:int32 = UnboxInt32
// CHECK-NEXT: [[STRING:v[0-9]+]] = IntegerToString ([[INTEGER]])
// CHECK-NEXT: BranchTaggedNotEquals ([[STRING]], #empty)
// CHECK: [[SUM:v[0-9]+]] = ConcatenateStrings
// CHECK-NEXT: BranchTaggedNotEquals ([[SUM]], #empty)
// CHECK: CallSlowPath Add
// CHECK: [[EQUAL:v[0-9]+]] = StringsEqual
// CHECK-NEXT: BranchTaggedNotEquals ([[EQUAL]], #empty)
// CHECK: CallSlowPath StrictlyEquals
// CHECK: BranchNullish ([[A]])
