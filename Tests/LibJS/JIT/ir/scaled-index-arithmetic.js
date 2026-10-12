// The heap index arithmetic of asm.js code, `(base + (i << 2)) >> 2`, is `i + base / 4` where the range of `i` keeps the
// shift and the addition from overflowing and the base is a multiple of 4.

const heap = new Int32Array(4096);
function sum() {
    let i = 0;
    let total = 0;
    do {
        total = (total + heap[(400 + (i << 2)) >> 2]) | 0;
        i = (i + 1) | 0;
    } while ((i | 0) != 1024);
    return total;
}
jit.prepare(sum);
for (let k = 0; k < 10; ++k) sum();
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: loop invariant code motion
// CHECK: Int32RightShift
// CHECK-PASS: bounds check elimination
// CHECK-NOT: Int32RightShift
// CHECK: Int32WrappingAdd ({{v[0-9]+}}, #i32:100)
