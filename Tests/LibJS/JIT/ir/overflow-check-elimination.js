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

// A bounds check against a constant count, like a fixed length typed array's, bounds the index too.

const table = new Int32Array(256);
function lookup(i) {
    return table[i] + (i + 1);
}
jit.prepare(lookup);
for (let k = 0; k < 10; ++k) lookup(k);
jit.compile(lookup);

// CHECK-FUNCTION: lookup
// CHECK-PASS: bounds check elimination
// CHECK: CheckBounds ({{v[0-9]+}}, #i32:256)
// CHECK: Int32WrappingAdd ({{v[0-9]+}}, #i32:1)

// A branch on an index being in bounds bounds it where it holds, like the array iterator's index, which it then
// increments.

function total(a) {
    let s = 0;
    for (const x of a) s = (s + x) | 0;
    return s;
}
jit.prepare(total);
for (let k = 0; k < 10; ++k) total([1, 2, 3, , 5]);
jit.compile(total);

// CHECK-FUNCTION: total
// CHECK-PASS: bounds check elimination
// CHECK: RefineIndexInBounds
// CHECK: [[NEXT:v[0-9]+]]:int32 = Int32WrappingAdd ({{v[0-9]+}}, #i32:1)
// CHECK-NEXT: StoreArrayIteratorIndex ({{v[0-9]+}}, [[NEXT]])

// An unsigned right shift of any int32 by at least one is a non-negative int32, like the bytes of hashes.

function mix(h, x) {
    return (h >>> 24) * 3 + (x & 0xff);
}
jit.prepare(mix);
for (let k = 0; k < 10; ++k) mix(k * 0x1234567, k);
jit.compile(mix);

// CHECK-FUNCTION: mix
// CHECK-PASS: bounds check elimination
// CHECK: Int32WrappingMul ({{v[0-9]+}}, #i32:3)
// CHECK: Int32WrappingAdd
