// Number checks of boxed int32 values, which representation selection leaves where an int32 phi meets a user of
// numbers (like a store to an Int32Array), become conversions, which then cancel out with the conversion back.

const heap = new Int32Array(64);
function storeEither(a, b) {
    a = a | 0;
    b = b | 0;
    let x = 0;
    if ((a | 0) > (b | 0)) x = (a - b) | 0;
    else x = (b + 1) | 0;
    heap[a & 15] = x;
    return 0;
}
jit.prepare(storeEither);
for (let i = 0; i < 10; ++i) storeEither(i, 3);
jit.compile(storeEither);

// CHECK-FUNCTION: storeEither
// CHECK-PASS: graph building
// CHECK: = CheckNumber
// CHECK-NEXT: = Float64ToInt32
// CHECK-PASS: simplification#2
// CHECK: [[X:v[0-9]+]]:int32 = Phi
// CHECK-NOT: CheckNumber
// CHECK-NOT: Float64ToInt32
// CHECK: StoreElementAt TypedArray(Int32) ({{v[0-9]+}}, {{v[0-9]+}}, [[X]])
