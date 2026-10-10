// A value checked to be an int32 once needs no second check.

function twice(a) {
    return a + 1 + (a + 2);
}
jit.prepare(twice);
for (let i = 0; i < 10; ++i) twice(i);
jit.compile(twice);

// CHECK-FUNCTION: twice
// CHECK-PASS: graph building
// CHECK-COUNT-2: CheckInt32
// CHECK-PASS: global value numbering
// CHECK-COUNT-1: CheckInt32
// CHECK: [[A:v[0-9]+]]:int32 = CheckInt32
// CHECK: Int32Add ([[A]], #i32:1)
// CHECK: Int32Add ([[A]], #i32:2)
