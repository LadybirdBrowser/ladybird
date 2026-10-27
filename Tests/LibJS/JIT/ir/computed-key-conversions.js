// The key conversion of a compound assignment calls into the runtime only for objects, in a cold block. For an
// int32 key, the check goes away.

function increment(array, n) {
    for (let i = 0; i < n; ++i) array[i] += 1;
    return array;
}
jit.prepare(increment);
for (let i = 0; i < 10; ++i) increment([0, 0, 0, 0], 4);
jit.compile(increment);

// CHECK-FUNCTION: increment
// CHECK-PASS: graph building
// CHECK: BranchObject ([[KEY:v[0-9]+]]) -> [[COLD:b[0-9]+]], {{b[0-9]+}}
// CHECK: [[COLD]] <- {{b[0-9]+}} cold:
// CHECK-NEXT: CallSlowPath ToPrimitiveWithStringHint {{@[0-9]+}} ([[KEY]])
// CHECK-PASS: frame initialization
// CHECK-NOT: ToPrimitiveWithStringHint
