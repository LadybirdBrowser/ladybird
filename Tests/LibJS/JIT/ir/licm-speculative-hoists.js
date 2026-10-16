// Checks in blocks that not every iteration runs move out of the loop too, speculatively. One that fails there exits
// with its own reason, Hoisted, so the code compiled next keeps such checks in the loop, and still moves those that
// every iteration runs.

function sum(object, other, n) {
    let total = 0;
    for (let i = 0; i < n; ++i) {
        total = (total + object.x) | 0;
        if (i === 7) total = (total + other.y) | 0;
    }
    return total;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum({ x: i }, { y: 1 }, 10);
jit.compile(sum);
sum({ x: 1 }, { z: 0, y: 2 }, 3);
jit.discard(sum);
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: loop invariant code motion
// CHECK: CheckShape
// CHECK-NEXT: CellAddress
// CHECK-NEXT: LoadNamed +0
// CHECK-NOT: CheckShape
// CHECK: loop:
// CHECK: CheckShape

console.log(sum({ x: 2 }, { y: 3 }, 10));
