// Array.prototype.push of several values appends them all inline where the
// elements have room for them, and calls the runtime for each otherwise.

function fill(n) {
    const a = [];
    for (let i = 0; i < n; ++i) a.push(i, -i, 0);
    return a;
}
jit.prepare(fill);
for (let i = 0; i < 10; ++i) fill(20);
jit.compile(fill);
if (fill(50).length !== 150) throw new Error("wrong length");

// CHECK-FUNCTION: fill
// CHECK-PASS: graph building
// CHECK: [[CHECKED:v[0-9]+]] = CheckAppendableArray ([[RECEIVER:v[0-9]+]])
// CHECK-NEXT: [[ARRAY:v[0-9]+]]:pointer = CellAddress ([[CHECKED]])
// CHECK-NEXT: [[LENGTH:v[0-9]+]]:int32 = LoadElementsLength ([[ARRAY]])
// CHECK-NEXT: [[CAPACITY:v[0-9]+]]:int32 = LoadElementsCapacity ([[ARRAY]])
// CHECK-NEXT: [[ROOM:v[0-9]+]]:int32 = Int32Sub ([[CAPACITY]], [[LENGTH]])
// CHECK-NEXT: BranchInt32GreaterThanEquals ([[ROOM]], #i32:3) -> [[FITS:b[0-9]+]], [[GROW:b[0-9]+]]
// CHECK: [[FITS]] <- {{b[0-9]+}}:
// CHECK: [[FIRST:v[0-9]+]]:int32 = AppendElement ([[ARRAY]], [[LENGTH]],
// CHECK-NEXT: [[SECOND:v[0-9]+]]:int32 = AppendElement ([[ARRAY]], [[FIRST]],
// CHECK-NEXT: [[THIRD:v[0-9]+]]:int32 = AppendElement ([[ARRAY]], [[SECOND]],
// CHECK-NEXT: BoxInt32 ([[THIRD]])
// CHECK: [[GROW]] <- {{b[0-9]+}} cold:
// CHECK-NEXT: CallArrayPush ([[CHECKED]],
// CHECK-NEXT: CallArrayPush ([[CHECKED]],
// CHECK-NEXT: CallArrayPush ([[CHECKED]],
