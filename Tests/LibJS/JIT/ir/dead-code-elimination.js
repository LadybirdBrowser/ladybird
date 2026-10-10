// Values nothing uses are removed, but checks that may exit stay.

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
// CHECK: Int32Add
// CHECK: Int32Mul
// CHECK: Return
