// OPTIONS: stress-osr
// Bounds checks of indices that a counted loop's induction variable keeps within the count go away. The counter that
// an on-stack replacement entry brings is checked against the loop's bound once where it enters instead.

const heap = new Int32Array(4096);
function fill(value) {
    value = value | 0;
    let i = 0;
    do {
        heap[(400 + (i << 2)) >> 2] = value;
        i = (i + 1) | 0;
    } while ((i | 0) != 1024);
}
jit.prepare(fill);
for (let k = 0; k < 10; ++k) fill(k);
jit.compile(fill);

// CHECK-FUNCTION: fill
// CHECK-PASS: loop invariant code motion
// CHECK: CheckBounds ({{v[0-9]+}}, #i32:4096)
// CHECK-PASS: bounds check elimination
// CHECK: CheckBounds ({{v[0-9]+}}, #i32:1024)
// CHECK: loop:
// CHECK-NOT: CheckBounds
// CHECK: StoreElementAt
