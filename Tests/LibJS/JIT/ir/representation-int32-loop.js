// Int32 loop counters and sums stay unboxed int32 values in phis.

function count(n) {
    let sum = 0;
    for (let i = 0; i < n; ++i) sum = (sum + i) | 0;
    return sum;
}
jit.prepare(count);
for (let i = 0; i < 10; ++i) count(20);
jit.compile(count);

// CHECK-FUNCTION: count
// CHECK-PASS: frame initialization
// CHECK: loop:
// CHECK-NEXT: [[I:v[0-9]+]]:int32 = Phi (#i32:0, [[NEXT:v[0-9]+]])
// CHECK-NEXT: [[SUM:v[0-9]+]]:int32 = Phi (#i32:0, {{v[0-9]+}})
// CHECK: BranchInt32LessThan ([[I]],
// CHECK: Int32WrappingAdd ([[SUM]], [[I]])
// CHECK: [[NEXT]]:int32 = Int32Add ([[I]], #i32:1)
