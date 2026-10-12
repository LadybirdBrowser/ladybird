// For-of loops over arrays make array iterators without calling the
// iterator method, and step them inline.

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
// CHECK: [[ITERATOR:v\d+]] = CreateArrayIterator
// CHECK: BranchArrayIterator
// CHECK: LoadArrayIteratorArray
// CHECK: LoadArrayIteratorIndex
// CHECK: LoadElementAt Holey
// CHECK: StoreArrayIteratorIndex
