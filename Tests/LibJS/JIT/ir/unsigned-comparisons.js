// Comparisons of uint32 values compare the bits of the shifted int32 values unsigned, so that the `>>> 0` shifts
// neither exit for values above 2^31 nor make the comparison one of doubles.

function compareUnsigned(a, b) {
    a = a | 0;
    b = b | 0;
    let result = 0;
    if (a >>> 0 < b >>> 0) result = 1;
    if (((a - 8) | 0) >>> 0 < 20) result = (result + 2) | 0;
    return result;
}
jit.prepare(compareUnsigned);
for (let i = 0; i < 10; ++i) {
    compareUnsigned(i, 3);
    compareUnsigned(-i, 3);
}
jit.compile(compareUnsigned);

// CHECK-FUNCTION: compareUnsigned
// CHECK-PASS: graph building
// CHECK: Uint32ToFloat64
// CHECK: Float64LessThan
// CHECK-PASS: truncation
// CHECK: Int32Below
// CHECK: Int32Below ({{v[0-9]+}}, #i32:20)
// CHECK-PASS: dead code elimination
// CHECK-NOT: Uint32
// CHECK-NOT: UnsignedRightShift
// CHECK: frame states:
