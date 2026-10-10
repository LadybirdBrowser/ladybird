// The int32 check of a loop bound the loop does not change moves out of the loop, ahead of its header.

function total(object, count) {
    let sum = 0;
    for (let i = 0; i < count; ++i) sum = (sum + object.value) | 0;
    return sum;
}
jit.prepare(total);
for (let i = 0; i < 10; ++i) total({ value: i }, 10);
jit.compile(total);

// CHECK-FUNCTION: total
// CHECK-PASS: global value numbering
// CHECK: loop:
// CHECK: CheckInt32
// CHECK-PASS: loop invariant code motion
// CHECK: [[COUNT:v[0-9]+]] = LoadSlot a1
// CHECK-NEXT: [[BOUND:v[0-9]+]]:int32 = CheckInt32 ([[COUNT]])
// CHECK-NOT: CheckInt32 ([[COUNT]])
// CHECK: loop:
// CHECK: Int32LessThan ({{v[0-9]+}}, [[BOUND]])
