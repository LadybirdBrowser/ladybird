// Top-level code runs once, so the code a loop in it gets compiled for is only entered at the loop: what only runs
// before it, like a large literal, is left out.

var table = [
    [1, 2],
    [3, 4],
    [5, 6],
];
var sum = 0;
for (var i = 0; i < 10000; ++i) sum += table[i % 3][0];
if (sum !== 29998) throw new Error("The loop computed the wrong sum");

// CHECK-FUNCTION: <anonymous>
// CHECK-PASS: graph building
// CHECK: Unreachable
// CHECK-NOT: AllocateArray
// CHECK: osr entry at pc
