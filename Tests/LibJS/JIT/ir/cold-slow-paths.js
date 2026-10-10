// Checks built as IR branch to cold blocks that call the instruction's slow
// path, and the code after them continues where the check passed.

function sum(o) {
    const { a, b } = o;
    return a + b;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum({ a: i, b: 2 });
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: graph building
// CHECK: BranchNullish ([[O:v[0-9]+]]) -> [[COLD:b[0-9]+]], [[HOT:b[0-9]+]]
// CHECK: [[COLD]] <- {{b[0-9]+}} cold:
// CHECK-NEXT: CallSlowPath ThrowIfNullish {{@[0-9]+}} ([[O]])
// CHECK-NEXT: Unreachable
// CHECK-PASS: frame initialization
// CHECK: Return
// CHECK: cold:
// CHECK-NEXT: CallSlowPath ThrowIfNullish

// A binding read before its declaration checks a constant: the branch folds,
// and the code goes straight to the cold block, which throws.

function late(n) {
    let s = 0;
    for (let i = 0; i < n; ++i) {
        if (i > 3) s += z;
    }
    let z = 2;
    return s + z;
}
jit.prepare(late);
for (let i = 0; i < 10; ++i) late(2);
jit.compile(late);

// CHECK-FUNCTION: late
// CHECK-PASS: graph building
// CHECK: BranchTaggedEquals (#empty, #empty)
// CHECK: CallSlowPath ThrowIfTDZ
// CHECK-PASS: simplification
// CHECK-NOT: BranchTaggedEquals (#empty, #empty)
// CHECK: cold:
// CHECK-NEXT: CallSlowPath ThrowIfTDZ
