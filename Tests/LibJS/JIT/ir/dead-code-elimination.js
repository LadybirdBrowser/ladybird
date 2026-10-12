// Values nothing uses are removed, but checks that may exit stay. The unused sum and product go too: int32 arithmetic
// only exits to produce results that are no int32, which nothing reads here.

function unused(a, b) {
    const sum = a + b;
    const product = a * b;
    return a;
}
jit.prepare(unused);
for (let i = 0; i < 10; ++i) unused(i, 2);
jit.compile(unused);

// CHECK-FUNCTION: unused
// CHECK-PASS: graph building
// CHECK: LoadSlot r2
// CHECK-COUNT-4: CheckInt32
// CHECK-PASS: frame initialization
// CHECK-NOT: LoadSlot r2
// CHECK-COUNT-2: CheckInt32
// CHECK-NOT: Add (
// CHECK-NOT: Mul (
// CHECK: Return
