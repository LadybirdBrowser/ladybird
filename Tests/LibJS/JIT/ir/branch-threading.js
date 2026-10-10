// A jump on a comparison whose paths join with a phi of booleans branches on each path's own comparison instead, which
// then fuses with the branch: compare and jump for int32 values and doubles, and the slow path's jump target.

function count(values, limit) {
    let n = 0;
    for (let i = 0; i < values.length; ++i) if (values[i] < limit) ++n;
    return n;
}
jit.prepare(count);
const values = [1, 2.5, 3, 4.5, 5, 6.5];
for (let i = 0; i < 10; ++i) count(values, 4);
jit.compile(count);

// CHECK-FUNCTION: count
// CHECK-PASS: branch fusion
// CHECK: BranchInt32LessThan ({{v[0-9]+}}, {{v[0-9]+}})
// CHECK: BranchFloat64LessThan ({{v[0-9]+}}, {{v[0-9]+}})
// CHECK: CallSlowPath JumpLessThan
// CHECK-NEXT: BranchInt32StrictlyEquals
// CHECK-NOT: BranchBool
