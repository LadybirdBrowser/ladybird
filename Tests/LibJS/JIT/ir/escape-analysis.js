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

// An object that a frame state refers to where its shape isn't known is allocated. Here, the code before the outer
// loop exits, since it has no feedback, so only the back edge reaches the loop's header (the loop is entered through
// the resume entry in the inner loop) and no shape reaches it either. But the header's frame state refers to the
// object the previous iteration allocated.

function* resumedLoop(a) {
    let s = a + 1;
    let o;
    for (let k = 0; k < 1000; ++k) {
        for (let j = 0; j < 50; ++j) yield j;
        o = { a: k, b: s };
    }
    return o.a;
}
const resumed = resumedLoop(1);
resumed.next();
jit.prepare(resumedLoop);
for (let i = 0; i < 200; ++i) resumed.next();
jit.compile(resumedLoop);

// CHECK-FUNCTION: resumedLoop
// CHECK-PASS: escape analysis
// CHECK: AllocateObject
