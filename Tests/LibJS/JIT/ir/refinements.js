// Checks and branches refine the values they test: the nodes that rely on a
// check take its refined value, so that passes only move or share them with
// the check. After the passes, refinements are gone: every use of a refined
// value is a use of the value itself.

function sumOfX(o) {
    let s = 0;
    for (let i = 0; i < 10; ++i) s += o.x;
    return s;
}
jit.prepare(sumOfX);
for (let i = 0; i < 10; ++i) sumOfX({ x: i });
jit.compile(sumOfX);

// CHECK-FUNCTION: sumOfX
// CHECK-PASS: graph building
// CHECK: [[OBJECT:v[0-9]+]] = CheckObject ([[O:v[0-9]+]])
// CHECK-NEXT: [[ADDRESS:v[0-9]+]]:pointer = CellAddress ([[OBJECT]])
// CHECK-NEXT: [[SHAPED:v[0-9]+]] = CheckShape {{\[[^]]*\]}} ([[OBJECT]], [[ADDRESS]])
// CHECK: LoadNamed +{{[0-9]+}} ([[SHAPED]],
// CHECK-PASS: loop invariant code motion
// CHECK: [[HOISTED:v[0-9]+]] = CheckShape
// CHECK: LoadNamed +{{[0-9]+}} ([[HOISTED]],
// CHECK-PASS: refinement removal
// CHECK-NOT: = CheckObject
// CHECK-NOT: = CheckShape
// CHECK-NOT: Refine

// A branch to a cold block refines its input on the edge where the code
// continues, at the head of that block.

function code(s, i) {
    return s.charCodeAt(i);
}
jit.prepare(code);
for (let i = 0; i < 10; ++i) code("hello", i % 5);
jit.compile(code);

// CHECK-FUNCTION: code
// CHECK-PASS: graph building
// CHECK: BranchString ([[S:v[0-9]+]]) -> [[STRING:b[0-9]+]], {{b[0-9]+}}
// CHECK: [[STRING]] <- {{b[0-9]+}}:
// CHECK-NEXT: RefineString ([[S]])
// CHECK-PASS: refinement removal
// CHECK-NOT: Refine

console.log(sumOfX({ x: 3 }), code("abc", 1));

// Loads that a branch guards stay after it: in a loop over a string that
// does not change, the code unit load takes the refinements of the branches
// on the string and the index, which stay in the loop, so loop invariant
// code motion cannot move the load out of it.

function firstCodes(s, n) {
    let sum = 0;
    for (let i = 0; i < n; ++i) sum += s.charCodeAt(0);
    return sum;
}
jit.prepare(firstCodes);
for (let i = 0; i < 10; ++i) firstCodes("abc", 5);
jit.compile(firstCodes);

// CHECK-FUNCTION: firstCodes
// CHECK-PASS: loop invariant code motion
// CHECK: {{b[0-9]+}} {{.*}}loop:
// CHECK: [[READABLE:v[0-9]+]]:pointer = RefineResolvedString
// CHECK: [[INDEX:v[0-9]+]]:int32 = RefineIndexInBounds
// CHECK-NEXT: LoadStringCodeUnit ([[READABLE]], [[INDEX]])

console.log(firstCodes("abc", 3));
