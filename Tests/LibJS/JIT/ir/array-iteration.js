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
// CHECK-PASS: frame initialization
// CHECK: [[ITERATOR:v\d+]] = CreateArrayIterator
// CHECK: BranchArrayIterator
// CHECK: LoadArrayIteratorArray
// CHECK: LoadArrayIteratorIndex
// CHECK: LoadElementAt Holey
// CHECK: StoreArrayIteratorIndex
