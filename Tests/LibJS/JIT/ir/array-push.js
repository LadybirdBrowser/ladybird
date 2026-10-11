// Array.prototype.push of one value is built as IR: a check that the array
// appends without observable steps, then the value becomes the next element
// where the elements have room for it, and cold blocks call the runtime to
// grow them otherwise.

function fill(n) {
    const a = [];
    for (let i = 0; i < n; ++i) a.push(i);
    return a;
}
jit.prepare(fill);
for (let i = 0; i < 10; ++i) fill(20);
jit.compile(fill);

// CHECK-FUNCTION: fill
// CHECK-PASS: graph building
// CHECK: [[CHECKED:v[0-9]+]] = CheckAppendableArray ([[RECEIVER:v[0-9]+]])
// CHECK-NEXT: [[ARRAY:v[0-9]+]]:pointer = CellAddress ([[CHECKED]])
// CHECK-NEXT: [[LENGTH:v[0-9]+]]:int32 = LoadElementsLength ([[ARRAY]])
// CHECK-NEXT: [[CAPACITY:v[0-9]+]]:int32 = LoadElementsCapacity ([[ARRAY]])
// CHECK-NEXT: BranchIndexInBounds ([[LENGTH]], [[CAPACITY]]) -> [[ROOM:b[0-9]+]], [[GROW:b[0-9]+]]
// CHECK: [[ROOM]] <- {{b[0-9]+}}:
// CHECK-NEXT: [[INDEX:v[0-9]+]]:int32 = RefineIndexInBounds ([[LENGTH]], [[CAPACITY]])
// CHECK-NEXT: [[NEW:v[0-9]+]]:int32 = AppendElement ([[ARRAY]], [[INDEX]], [[VALUE:v[0-9]+]])
// CHECK-NEXT: BoxInt32 ([[NEW]])
// CHECK: [[GROW]] <- {{b[0-9]+}} cold:
// CHECK-NEXT: CallArrayPush ([[CHECKED]], [[VALUE]])

console.log(fill(100).length);
