// Doubles that feedback only saw holding int32 values are compared, converted and passed on as doubles, rather than
// boxed and checked to be int32 values first: the doubles give the same results, also where they hold no int32.

const keys = new Float64Array([1, 2, 3, 4, 5, 6, 7, 8]);
function countBelow(limit) {
    let count = 0;
    for (let i = 0; i < 8; ++i) {
        if (keys[i] < limit) count = (count + 1) | 0;
    }
    return count;
}
jit.prepare(countBelow);
for (let k = 0; k < 10; ++k) countBelow(k);
jit.compile(countBelow);

// CHECK-FUNCTION: countBelow
// CHECK-PASS: graph building
// CHECK: [[KEY:v[0-9]+]]:float64 = LoadElementAt TypedArray(Float64)
// CHECK-NEXT: [[BOXED:v[0-9]+]] = BoxFloat64 ([[KEY]])
// CHECK: CheckInt32 ([[BOXED]])
// CHECK-PASS: dead code elimination#2
// CHECK: Float64LessThan ([[KEY]],
// CHECK-NOT: BoxFloat64 ([[KEY]])
// CHECK: frame states:
