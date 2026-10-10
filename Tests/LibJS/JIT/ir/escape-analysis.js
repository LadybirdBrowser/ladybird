// An object that does not escape is never allocated: its properties become values.

function distance(x, y) {
    const point = { x, y };
    return point.x * point.x + point.y * point.y;
}
function escapes(x) {
    const point = { x };
    return point;
}
jit.prepare(distance);
jit.prepare(escapes);
for (let i = 0; i < 10; ++i) {
    distance(i, i + 1);
    escapes(i);
}
jit.compile(distance);
jit.compile(escapes);

// CHECK-FUNCTION: distance
// CHECK-PASS: graph building
// CHECK: AllocateObject
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateObject
// CHECK-NOT: LoadNamed
// CHECK: [[X:v[0-9]+]]:int32 = CheckInt32
// CHECK-NEXT: Int32Mul ([[X]], [[X]])

// CHECK-FUNCTION: escapes
// CHECK-PASS: escape analysis
// CHECK: [[OBJECT:v[0-9]+]] = AllocateObject
// CHECK: InitializeNamed +0 ([[OBJECT]],
// CHECK: Return ([[OBJECT]])

// An object held by one that does not escape is allocated when a loop merges the property that holds it: frame states
// in the loop describe the outer object with the inner one as a value.

function nested(values) {
    const outer = { inner: {} };
    let sum = 0;
    for (const value of values) sum += value;
    return sum;
}
jit.prepare(nested);
for (let i = 0; i < 10; ++i) nested([i, 1]);
jit.compile(nested);

// CHECK-FUNCTION: nested
// CHECK-PASS: escape analysis
// CHECK: AllocateObject 0x{{[0-9a-f]+}} with 0 properties
// CHECK-NOT: AllocateObject
// CHECK: CallSlowPath GetIterator
