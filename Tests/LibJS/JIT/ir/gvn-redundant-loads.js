// Global value numbering keeps one load of a property read twice with nothing in between that could change it.

function sum(object) {
    return object.x + object.x;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum({ x: i });
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: graph building
// CHECK-COUNT-2: CheckShape
// CHECK-COUNT-2: LoadNamed +0
// CHECK-PASS: global value numbering#2
// CHECK-COUNT-1: CheckShape
// CHECK-COUNT-1: LoadNamed
// CHECK: [[VALUE:v[0-9]+]] = LoadNamed +0
// CHECK: [[INT:v[0-9]+]]:int32 = CheckInt32 ([[VALUE]])
// CHECK-NEXT: Int32Add ([[INT]], [[INT]])
