// The next key of a for-in loop is built as IR: while the property iterator
// cache still holds the object's keys, the key at the cursor comes from the
// cache, and a cold block runs the slow path, whose outputs are SSA values,
// otherwise.

function keys(o) {
    let s = "";
    for (const k in o) s += k;
    return s;
}
jit.prepare(keys);
for (let i = 0; i < 10; ++i) keys({ a: 1, b: 2, c: 3 });
jit.compile(keys);

// CHECK-FUNCTION: keys
// CHECK-PASS: graph building
// CHECK: BranchObject ([[RECEIVER:v[0-9]+]])
// CHECK: [[OBJECT:v[0-9]+]] = RefineObject ([[RECEIVER]])
// CHECK-NEXT: BranchPropertyIteratorCacheValid ([[OBJECT]], [[CACHE:v[0-9]+]])
// CHECK: BranchNonNegativeInt32 ([[CURSOR:v[0-9]+]])
// CHECK: [[REFINED:v[0-9]+]] = RefineNonNegativeInt32 ([[CURSOR]])
// CHECK-NEXT: [[INDEX:v[0-9]+]]:int32 = UnboxInt32 ([[REFINED]])
// CHECK-NEXT: [[COUNT:v[0-9]+]]:int32 = LoadPropertyIteratorKeyCount ([[CACHE]])
// CHECK-NEXT: BranchInt32LessThan ([[INDEX]], [[COUNT]])
// CHECK: [[IN_BOUNDS:v[0-9]+]]:int32 = RefineInt32LessThan ([[INDEX]], [[COUNT]])
// CHECK-NEXT: LoadPropertyIteratorKey ([[CACHE]], [[IN_BOUNDS]])
// CHECK: [[CALL:v[0-9]+]] = CallSlowPath ObjectPropertyIteratorNext {{@[0-9]+}} ([[RECEIVER]], [[CACHE]], [[CURSOR]])
// CHECK-NEXT: SlowPathOutput 1 ([[CALL]])
// CHECK-NEXT: SlowPathOutput 2 ([[CALL]])

console.log(keys({ x: 1, y: 2 }));
