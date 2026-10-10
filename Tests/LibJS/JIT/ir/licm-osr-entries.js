// OPTIONS: stress-osr
// Loops with their test at the bottom are entered at their header by on-stack replacement entries too, and loop
// invariant code motion merges the entries in a preheader, here hoisting the check of the loop bound. Checks every
// iteration runs, like the lookup of the inlined callee, move there as well.

// NB: The callee is assigned after its variable held another value, so that the loop looks it up rather than
//     taking it as a constant.
var first = null;
first = function (a, b) {
    return a;
};
function callLoop(n) {
    let s = 0;
    for (let i = 0; i < n; ++i) s = first(i, s);
    return s;
}
jit.prepare(first);
jit.prepare(callLoop);
for (let i = 0; i < 10; ++i) callLoop(5);
jit.compile(callLoop);

// CHECK-FUNCTION: callLoop
// CHECK-PASS: graph building
// CHECK: (pc {{[0-9]+}}) <- {{b[0-9]+}}, {{b[0-9]+}}, {{b[0-9]+}} loop:
// CHECK: osr entry at pc
// CHECK-PASS: loop invariant code motion
// CHECK: = Phi ({{v[0-9]+}}, #i32:0)
// CHECK: [[BOUND:v[0-9]+]] = LoadSlot a0
// CHECK-NEXT: CheckInt32 ([[BOUND]])
// CHECK-NEXT: CheckShape
// CHECK-NEXT: LoadNamed
// CHECK-NEXT: CheckValue
// CHECK-NEXT: Jump [[HEADER:b[0-9]+]]
// CHECK-NEXT: [[HEADER]] (pc {{[0-9]+}}) <- {{b[0-9]+}}, {{b[0-9]+}} loop:
// CHECK-NOT: CheckShape
// CHECK: osr entry at pc
