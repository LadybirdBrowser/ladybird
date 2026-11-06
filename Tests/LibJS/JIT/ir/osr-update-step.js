// OPTIONS: stress-osr
// A continue reaches the update step of a loop going forward, like the end of the body, so that the loop has one back
// edge and one on-stack replacement entry, at its test. That gives it one header, and loop invariant code motion can
// hoist the loads of the loop bound and of the object's property.

function sumOf(object, n) {
    let sum = 0;
    for (let i = 0; i < n; ++i) {
        const x = object.x;
        if (i === 2) continue;
        sum = (sum + x) | 0;
    }
    return sum;
}
jit.prepare(sumOf);
for (let i = 0; i < 10; ++i) sumOf({ x: i }, 5);
jit.compile(sumOf);

// CHECK-FUNCTION: sumOf
// CHECK-PASS: graph building
// CHECK: b0:
// CHECK: LoadSlot a1
// CHECK-NEXT: Jump [[HEADER:b[0-9]+]]
// CHECK: [[HEADER]] (pc {{[0-9]+}}) <- {{b[0-9]+}}, {{b[0-9]+}}, {{b[0-9]+}} loop:
// CHECK: Int32Add ({{v[0-9]+}}, #i32:1)
// CHECK-NEXT: BoxInt32
// CHECK-NEXT: Jump [[HEADER]]
// CHECK-COUNT-1: osr entry at pc
// CHECK-PASS: loop invariant code motion
// CHECK: CheckShape
// CHECK-NEXT: CellAddress
// CHECK-NEXT: LoadNamed +0
// CHECK: loop:
// CHECK-NOT: LoadNamed
// CHECK: osr entry at pc

console.log(sumOf({ x: 3 }, 5));

// A loop whose test is a constant ends in a back edge that jumps to the start of the body. A body of one block leads
// to the test, which is the loop's header, so the on-stack replacement entry at that back edge runs the body itself
// and continues at the header, and the loop stays reducible.

function stopAt(i, n) {
    if (i === n) throw 0;
}
function spin(object, n) {
    let sum = 0;
    try {
        for (let i = 0; true; ++i) {
            sum = (sum + object.x) | 0;
            stopAt(i, n);
        }
    } catch {
        return sum;
    }
}
jit.prepare(spin);
for (let i = 0; i < 10; ++i) spin({ x: i }, 5);
jit.compile(spin);

// CHECK-FUNCTION: spin
// CHECK-PASS: graph building
// CHECK: b1:
// CHECK: LoadNamed
// CHECK: Int32Add ({{v[0-9]+}}, #i32:1)
// CHECK-NEXT: BoxInt32
// CHECK-NEXT: Jump [[SPIN_HEADER:b[0-9]+]]
// CHECK: [[SPIN_HEADER]] (pc {{[0-9]+}}) <- {{b[0-9]+}}, {{b[0-9]+}}, {{b[0-9]+}} loop:

console.log(spin({ x: 3 }, 5));
