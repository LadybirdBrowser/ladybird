// Array length uses receiver feedback and guards, with no fallback call.
// In a pure scan this lets the length and the target property leave the loop.

function lengthOf(array) {
    return array.length;
}
function find(array, target) {
    for (let i = 0; i < array.length; ++i) {
        if (array[i].pos == target.pos) return array[i];
    }
    return false;
}
const position = {};
const array = [{ pos: {} }, { pos: position }];
jit.prepare(lengthOf);
jit.prepare(find);
for (let i = 0; i < 10; ++i) {
    lengthOf(array);
    find(array, { pos: position });
}
jit.compile(lengthOf);
jit.compile(find);
if (lengthOf(array) !== 2) throw new Error("wrong length");
if (find(array, { pos: position }) !== array[1]) throw new Error("wrong match");

// CHECK-FUNCTION: lengthOf
// CHECK-PASS: graph building
// CHECK: [[ARRAY:v[0-9]+]] = CheckArray ({{v[0-9]+}})
// CHECK-NEXT: [[ADDRESS:v[0-9]+]]:pointer = CellAddress ([[ARRAY]])
// CHECK-NEXT: [[LENGTH:v[0-9]+]]:int32 = LoadElementsLength ([[ADDRESS]])
// CHECK-NEXT: [[COUNT:v[0-9]+]]:int32 = CheckNonNegativeInt32 ([[LENGTH]])
// CHECK-NEXT: BoxInt32 ([[COUNT]])
// CHECK-NOT: CallSlowPath

// CHECK-FUNCTION: find
// CHECK-PASS: loop invariant code motion
// CHECK: CheckArray
// CHECK: LoadElementsLength
// CHECK: CheckNonNegativeInt32
// CHECK: [[POSITION:v[0-9]+]] = LoadNamed +0 ([[TARGET:v[0-9]+]], {{v[0-9]+}})
// CHECK: loop:
// CHECK-NOT: LoadElementsLength
// CHECK-NOT: LoadNamed +0 ([[TARGET]],
// CHECK-NOT: CallSlowPath
// CHECK: Return

function findInlined(array, target) {
    return find(array, target);
}
jit.prepare(findInlined);
for (let i = 0; i < 10; ++i) findInlined(array, { pos: position });
jit.compile(findInlined);
if (findInlined(array, { pos: position }) !== array[1]) throw new Error("wrong inlined match");
if (findInlined([], null) !== false) throw new Error("wrong empty inlined scan");

// CHECK-FUNCTION: findInlined
// CHECK-PASS: loop invariant code motion
// CHECK-NOT: CallDirect
// CHECK: CheckArray
// CHECK: LoadElementsLength
// CHECK: CheckNonNegativeInt32
// CHECK: LoadNamed +0
// CHECK: loop:
// CHECK-NOT: LoadElementsLength
// CHECK: TaggedEquals
// CHECK: fs{{[0-9]+}}: e1 pc {{[0-9]+}} at {{.*}} in fs{{[0-9]+}}
