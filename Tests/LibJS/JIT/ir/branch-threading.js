// A jump on a comparison whose paths join with a phi of booleans branches on each path's own comparison instead, which
// then fuses with the branch: compare and jump for int32 values and doubles, and the slow path's jump target. The
// feedback of the comparison saw other values than numbers, so it takes one path per kind of operand.

function count(values, limit) {
    let n = 0;
    for (let i = 0; i < values.length; ++i) if (values[i] < limit) ++n;
    return n;
}
jit.prepare(count);
const values = [1, 2.5, 3, null, 4.5, 5, 6.5];
for (let i = 0; i < 10; ++i) count(values, 4);
jit.compile(count);

// CHECK-FUNCTION: count
// CHECK-PASS: branch fusion
// CHECK: BranchInt32LessThan ({{v[0-9]+}}, {{v[0-9]+}})
// CHECK: BranchFloat64LessThan ({{v[0-9]+}}, {{v[0-9]+}})
// CHECK: CallSlowPath JumpLessThan
// CHECK-NEXT: BranchInt32StrictlyEquals
// CHECK-NOT: BranchBool

// A comparison whose feedback only saw numbers checks its operands to be numbers (the loop invariant limit once, before
// the loop) and branches on their comparison.

function countNumbers(values, limit) {
    let n = 0;
    for (let i = 0; i < values.length; ++i) if (values[i] < limit) ++n;
    return n;
}
jit.prepare(countNumbers);
const numbers = [1, 2.5, 3, 4.5, 5, 6.5];
for (let i = 0; i < 10; ++i) countNumbers(numbers, 4);
jit.compile(countNumbers);

// CHECK-FUNCTION: countNumbers
// CHECK-PASS: branch fusion
// CHECK: [[LIMIT:v[0-9]+]]:float64 = CheckNumber
// CHECK: loop:
// CHECK: [[VALUE:v[0-9]+]]:float64 = CheckNumber
// CHECK-NEXT: BranchFloat64LessThan ([[VALUE]], [[LIMIT]])
// CHECK-NOT: CallSlowPath JumpLessThan
// CHECK-NOT: BranchBool
