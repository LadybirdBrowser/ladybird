// OPTIONS: stress-osr
// Where an on-stack replacement entry also enters an inner loop, loop invariant code motion merges the entries in a
// preheader, after which the header phis of values the inner loop leaves as they are take only what the preheader
// brings. They go away, so that what the inner loop computes from them moves to the preheader.

const heap = new Int32Array(4096);
function fill(value) {
    value = value | 0;
    for (let row = 0; row < 4; row = (row + 1) | 0) {
        for (let column = 0; column < 16; column = (column + 1) | 0) heap[((row << 6) + (column << 2)) >> 2] = value;
    }
}
jit.prepare(fill);
for (let i = 0; i < 10; ++i) fill(i);
jit.compile(fill);

// CHECK-FUNCTION: fill
// CHECK-PASS: loop invariant code motion
// CHECK: Int32LeftShift ({{v[0-9]+}}, #i32:6)
// CHECK: loop:
// CHECK-NOT: #i32:6
// CHECK: StoreElementAt
