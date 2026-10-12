// Additions, subtractions and multiplications whose operands' ranges keep the result an int32, and multiplications
// that cannot make -0, stop checking for overflow. The ranges come from masks and shifts, and an index that a bounds
// check made below the elements' count can be incremented.

function multiplyDigits(x, y) {
    return (x & 0x3fff) * (y & 0x3fff) + ((x >> 14) - (y >> 14));
}
jit.prepare(multiplyDigits);
for (let k = 0; k < 10; ++k) multiplyDigits(k * 1000, k * 7000);
jit.compile(multiplyDigits);

// CHECK-FUNCTION: multiplyDigits
// CHECK-PASS: bounds check elimination
// CHECK: Int32WrappingMul
// CHECK: Int32WrappingSub
// CHECK: Int32WrappingAdd

function next(a, i) {
    return a[i] + (i + 1);
}
jit.prepare(next);
for (let k = 0; k < 10; ++k) next([1, 2, 3], k % 3);
jit.compile(next);

// CHECK-FUNCTION: next
// CHECK-PASS: bounds check elimination
// CHECK: CheckBounds
// CHECK: Int32WrappingAdd ({{v[0-9]+}}, #i32:1)

// The product of a value that may be negative and one that may be zero could be -0, and products of unknown ranges
// could overflow, so they stay checked.

function products(x, y) {
    return ((x >> 14) * (y & 0x3fff)) | (x * y);
}
jit.prepare(products);
for (let k = 0; k < 10; ++k) products(k * 1000, k * 7000);
jit.compile(products);

// CHECK-FUNCTION: products
// CHECK-PASS: bounds check elimination
// CHECK: Int32Mul
// CHECK: Int32Mul
// CHECK-NOT: Int32WrappingMul

// Every bounds check of an operand narrows its range: a typed array's length has no known range, but the array's
// count below it keeps `j + 1` from overflowing.

function afterBothAccesses(ta, a, i) {
    const j = i & 0x7fffffff;
    return ta[j] + a[j] + (j + 1);
}
jit.prepare(afterBothAccesses);
for (let k = 0; k < 10; ++k) afterBothAccesses(new Int32Array(4), [1, 2, 3, 4], k % 4);
jit.compile(afterBothAccesses);

// CHECK-FUNCTION: afterBothAccesses
// CHECK-PASS: bounds check elimination
// CHECK: CheckBounds
// CHECK: CheckBounds
// CHECK: Int32WrappingAdd ({{v[0-9]+}}, #i32:1)
