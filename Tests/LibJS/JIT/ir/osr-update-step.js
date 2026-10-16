// OPTIONS: stress-osr
// Loops with a continue have their update step in a block of its own, which the back edges jump to and which leads
// to the loop header. On-stack replacement entries there run the update step themselves and jump to the loop header,
// so that the loop has one header and loop invariant code motion can hoist the loads of the loop bound and of the
// object's property.

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
// CHECK: Int32Add ({{v[0-9]+}}, #i32:1)
// CHECK-NEXT: BoxInt32
// CHECK-NEXT: Jump [[HEADER]]
// CHECK: Int32Add ({{v[0-9]+}}, #i32:1)
// CHECK-NEXT: BoxInt32
// CHECK-NEXT: Jump [[HEADER]]
// CHECK: [[HEADER]] (pc {{[0-9]+}}) <- {{b[0-9]+}}, {{b[0-9]+}}, {{b[0-9]+}}, {{b[0-9]+}}, {{b[0-9]+}} loop:
// CHECK-PASS: loop invariant code motion
// CHECK: CheckShape
// CHECK-NEXT: CellAddress
// CHECK-NEXT: LoadNamed +0
// CHECK: loop:
// CHECK-NOT: LoadNamed
// CHECK: osr entry at pc

console.log(sumOf({ x: 3 }, 5));
