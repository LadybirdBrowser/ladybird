// Frame states take int32 values unboxed where only exits read their boxings, so that the boxings go away. The builder
// boxes the sum for the loop phi it flows into, and once representation selection unboxes the phi, only the exit of
// the increment after it reads the boxing.

function sumElements(array, n) {
    let sum = 0;
    for (let i = 0; i < n; ++i) sum += array[i];
    return sum;
}
jit.prepare(sumElements);
for (let i = 0; i < 10; ++i) sumElements([1, 2, 3, 4], 4);
jit.compile(sumElements);

// CHECK-FUNCTION: sumElements
// CHECK-PASS: loop invariant code motion
// CHECK: [[SUM:v[0-9]+]]:int32 = Int32Add
// CHECK-NEXT: [[BOXED:v[0-9]+]] = BoxInt32 ([[SUM]])
// CHECK: at {l0={{v[0-9]+}}, l1=[[BOXED]]}
// CHECK-PASS: exit unboxing
// CHECK: at {l0={{v[0-9]+}}, l1=[[SUM]]}
// CHECK-PASS: dead code elimination
// CHECK-NOT: = BoxInt32 ([[SUM]])
// CHECK: frame states:
