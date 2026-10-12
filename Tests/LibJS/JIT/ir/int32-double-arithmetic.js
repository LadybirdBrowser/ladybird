// Sums of int32 values whose feedback saw results beyond the int32 range are double arithmetic, but where only ToInt32
// reads them, like stores to an Int32Array, they become wrapping int32 arithmetic: doubles hold such sums exactly.

const heap = new Int32Array(4);
function storeSum(a, b, c) {
    a = a | 0;
    b = b | 0;
    c = c | 0;
    heap[0] = a + b - c;
}
jit.prepare(storeSum);
for (let i = 0; i < 10; ++i) storeSum(0x7fffffff, 0x7fffffff, i);
jit.compile(storeSum);

// CHECK-FUNCTION: storeSum
// CHECK-PASS: graph building
// CHECK: Float64Add
// CHECK: Float64Sub
// CHECK: Float64ToInt32
// CHECK-PASS: dead code elimination
// CHECK: [[SUM:v[0-9]+]]:int32 = Int32WrappingAdd
// CHECK-NEXT: [[DIFFERENCE:v[0-9]+]]:int32 = Int32WrappingSub ([[SUM]],
// CHECK-NOT: Float64ToInt32
// CHECK: StoreElementAt TypedArray(Int32) ({{v[0-9]+}}, {{.*}}, [[DIFFERENCE]])
