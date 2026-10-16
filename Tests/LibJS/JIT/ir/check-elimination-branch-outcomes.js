// A branch on values alone, like a comparison, goes the way a branch on the same values went before, where only one
// edge of that branch leads, and becomes a jump there.

function nested(a, b) {
    if (a < b) {
        if (a < b) return 1;
        return 2;
    }
    if (a < b) return 3;
    return 4;
}
jit.prepare(nested);
for (let i = 0; i < 10; ++i) nested(i, 5);
jit.compile(nested);

// CHECK-FUNCTION: nested
// CHECK-PASS: check elimination
// CHECK-COUNT-1: BranchBool
// CHECK: Return (#4)
// CHECK: Return (#1)

// Where both edges meet again, neither outcome is known.

function merged(a, b, c) {
    let s = 0;
    if (a < b) s = c;
    if (a < b) s = (s + 1) | 0;
    return s;
}
jit.prepare(merged);
for (let i = 0; i < 10; ++i) merged(i, 5, 7);
jit.compile(merged);

// CHECK-FUNCTION: merged
// CHECK-PASS: check elimination
// CHECK-COUNT-2: BranchBool

console.log(nested(1, 2), nested(2, 1), merged(1, 2, 3), merged(2, 1, 3));
