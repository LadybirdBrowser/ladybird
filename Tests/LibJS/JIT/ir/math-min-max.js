// Math.min and Math.max of two int32 values compare them inline.

function clamp(value, low, high) {
    return Math.max(low, Math.min(value, high));
}
jit.prepare(clamp);
for (let i = 0; i < 10; ++i) clamp(i, 2, 7);
jit.compile(clamp);
if (clamp(10, 2, 7) !== 7 || clamp(-3, 2, 7) !== 2 || clamp(5, 2, 7) !== 5) throw new Error("wrong clamp");

// CHECK-FUNCTION: clamp
// CHECK-PASS: frame initialization
// CHECK-NOT: CallNative
// CHECK: BranchInt32LessThan
// CHECK-NOT: CallNative
// CHECK: BranchInt32GreaterThan
// CHECK-NOT: CallNative
