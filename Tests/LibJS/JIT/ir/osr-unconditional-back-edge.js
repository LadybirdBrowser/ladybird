// OPTIONS: stress-osr
// On-stack replacement entries at unconditional back edges jump to the loop header, so that the loop has one header
// and loop invariant code motion can hoist the check of the loop bound to its preheader.

function countUp(n) {
    let i = 0;
    while (true) {
        i = (i + 1) | 0;
        if (i >= n) break;
    }
    return i;
}
jit.prepare(countUp);
for (let i = 0; i < 10; ++i) countUp(5);
jit.compile(countUp);

// CHECK-FUNCTION: countUp
// CHECK-PASS: graph building
// CHECK: b0:
// CHECK-NEXT: Jump
// CHECK-NEXT: [[ENTRY:b[0-9]+]]:
// CHECK-NEXT: LoadSlot l0
// CHECK-NEXT: LoadSlot a0
// CHECK-NEXT: Jump [[HEADER:b[0-9]+]]
// CHECK: [[HEADER]] (pc {{[0-9]+}}) <- [[ENTRY]], {{b[0-9]+}}, {{b[0-9]+}} loop:
// CHECK: osr entry at pc {{[0-9]+}}: [[ENTRY]]
// CHECK-PASS: loop invariant code motion
// CHECK: :int32 = Phi ({{v[0-9]+}}, #i32:0)
// CHECK-NEXT: [[BOUND:v[0-9]+]] = LoadSlot a0
// CHECK-NEXT: CheckInt32 ([[BOUND]])
// CHECK-NEXT: Jump [[LOOP:b[0-9]+]]
// CHECK-NEXT: [[LOOP]] (pc {{[0-9]+}}) <- {{b[0-9]+}}, {{b[0-9]+}} loop:
// CHECK-NOT: CheckInt32
// CHECK: osr entry at pc
