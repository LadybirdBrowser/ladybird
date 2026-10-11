// Math.min and Math.max of two int32 values compare them inline.

function clamp(value, low, high) {
    return Math.max(low, Math.min(value, high));
}
jit.prepare(clamp);
for (let i = 0; i < 10; ++i) clamp(i, 2, 7);
jit.compile(clamp);
if (clamp(10, 2, 7) !== 7 || clamp(-3, 2, 7) !== 2 || clamp(5, 2, 7) !== 5) throw new Error("wrong clamp");

// Constant arguments need no check, and constants of other kinds call the
// function.
function clampLow(value) {
    return Math.max(value, 0);
}
function clampHigh(value) {
    return Math.min(value, 2.5);
}
jit.prepare(clampLow);
jit.prepare(clampHigh);
for (let i = 0; i < 10; ++i) {
    clampLow(i);
    clampHigh(1);
}
jit.compile(clampLow);
jit.compile(clampHigh);
if (clampLow(-3) !== 0 || clampHigh(3) !== 2.5) throw new Error("wrong clamp");

// CHECK-FUNCTION: clamp
// CHECK-PASS: frame initialization
// CHECK-NOT: CallNative
// CHECK: BranchInt32LessThan
// CHECK-NOT: CallNative
// CHECK: BranchInt32GreaterThan
// CHECK-NOT: CallNative

// CHECK-FUNCTION: clampLow
// CHECK-PASS: frame initialization
// CHECK-NOT: CallNative
// CHECK-COUNT-1: CheckInt32
// CHECK: BranchInt32GreaterThan ({{v[0-9]+}}, #i32:0)

// CHECK-FUNCTION: clampHigh
// CHECK-PASS: frame initialization
// CHECK-NOT: CheckInt32
// CHECK: CallNative
