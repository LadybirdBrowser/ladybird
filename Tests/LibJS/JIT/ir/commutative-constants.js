// Commutative int32 operations take their constant operands on the right, where code generation uses them as
// immediates, like the constant bases of asm.js heap accesses.

function offset(i) {
    i = i | 0;
    return ((57352 + (i << 2)) | 0) & (3 * i);
}
jit.prepare(offset);
for (let k = 0; k < 10; ++k) offset(k);
jit.compile(offset);

// CHECK-FUNCTION: offset
// CHECK-PASS: graph building
// CHECK: Int32Add (#i32:57352,
// CHECK-PASS: simplification
// CHECK: Int32WrappingAdd ({{v[0-9]+}}, #i32:57352)
// CHECK: Int32Mul ({{v[0-9]+}}, #i32:3)
