// A comparison whose only use is a branch is fused into the branch.

function sign(a, b) {
    if (a < b) return -1;
    return 1;
}
jit.prepare(sign);
for (let i = 0; i < 10; ++i) sign(i, 5);
jit.compile(sign);

// CHECK-FUNCTION: sign
// CHECK-PASS: dead code elimination#2
// CHECK: [[LESS:v[0-9]+]]:bool = Int32LessThan ([[A:v[0-9]+]], [[B:v[0-9]+]])
// CHECK-NEXT: BranchBool ([[LESS]])
// CHECK-PASS: branch fusion
// CHECK-NOT: Int32LessThan
// CHECK: BranchInt32LessThan ([[A]], [[B]]) -> b{{[0-9]+}}, b{{[0-9]+}}
