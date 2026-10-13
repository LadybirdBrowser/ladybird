// For-of loops over arrays iterate them without array iterators and without
// calling the iterator method, and step them inline: the iterator operand
// holds the index of the next element.

function sum(array) {
    let total = 0;
    for (const value of array) total += value;
    return total;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum([1, 2, 3]);
jit.compile(sum);
if (sum([4, 5]) !== 9) throw new Error("wrong sum");

// CHECK-FUNCTION: sum
// CHECK-PASS: graph building
// CHECK: [[LENGTH:v\d+]]:int32 = LoadElementsLength
// CHECK-NEXT: BranchIndexInBounds ([[INDEX:v\d+]], [[LENGTH]])
// CHECK: [[CAPACITY:v\d+]]:int32 = LoadElementsCapacity
// CHECK-NEXT: BranchIndexInBounds ({{v\d+}}, [[CAPACITY]])
// CHECK-NOT: Uint32Min
// CHECK: LoadElementAt Holey
// CHECK-PASS: frame initialization
// CHECK: [[ARRAY:v\d+]] = OpenArrayIteration
// CHECK: BranchNonNegativeInt32
// CHECK: [[INDEX:v\d+]]:int32 = UnboxInt32
// CHECK: LoadElementAt Holey ({{v\d+}}, [[INDEX]])
// CHECK: BoxInt32
