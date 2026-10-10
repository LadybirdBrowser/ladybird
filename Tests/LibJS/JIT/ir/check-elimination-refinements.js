// A shape check that an earlier one in the loop makes redundant is removed, and the property load after it uses the
// earlier check's value, so it stays in the loop behind that check instead of moving out of the loop ahead of it.

function total(object, count) {
    let sum = 0;
    for (let i = 0; i < count; ++i) {
        if (i > 0) {
            sum = (sum + object.x) | 0;
            sum = (sum + object.y) | 0;
        }
    }
    return sum;
}
jit.prepare(total);
for (let i = 0; i < 10; ++i) total({ x: i, y: 1 }, 10);
jit.compile(total);

// CHECK-FUNCTION: total
// CHECK-PASS: loop invariant code motion
// CHECK: loop:
// CHECK: [[CHECKED:v[0-9]+]] = CheckShape
// CHECK: LoadNamed +0 ([[CHECKED]],
// CHECK-NOT: CheckShape
// CHECK: LoadNamed +1 ([[CHECKED]],
