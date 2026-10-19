// For-of loops over arrays iterate them without array iterators and without
// calling the iterator method, and step them inline: the iterator operand
// holds the index of the next element. Starting the loop checks inline that
// the realm still knows its arrays to be iterated with its array iterators,
// whose next method is the builtin one, and that the array is one of those,
// and asks the runtime where that fails.

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
// CHECK: BranchHoldsValidWatchpointSet (#pointer:{{[0-9a-fx]+}})
// CHECK: BranchHoldsValidWatchpointSet (#pointer:{{[0-9a-fx]+}})
// CHECK: BranchObject
// CHECK: [[ADDRESS:v\d+]]:pointer = CellAddress
// CHECK-NEXT: BranchArrayWithRealmArrayPrototypeAndNoNamedProperties ([[ADDRESS]])
// CHECK: OpenArrayIteration
// CHECK: BranchNonNegativeInt32
// CHECK: [[INDEX:v\d+]]:int32 = UnboxInt32
// CHECK: LoadElementAt Holey ({{v\d+}}, [[INDEX]])
// CHECK: BoxInt32

// An inlined method with a for-of loop over an array starts the loop without
// a call, every time.

class Totals {
    constructor() {
        this.total = 0;
    }
    add(values) {
        for (const value of values) this.total += value;
    }
}
function addTwice(totals, values) {
    totals.add(values);
    totals.add(values);
    return totals.total;
}
jit.prepare(Totals.prototype.add);
jit.prepare(addTwice);
for (let i = 0; i < 10; ++i) addTwice(new Totals(), [1, 2, 3]);
jit.compile(addTwice);
if (addTwice(new Totals(), [4, 5]) !== 18) throw new Error("wrong total");

// CHECK-FUNCTION: addTwice
// CHECK-PASS: frame initialization
// CHECK-NOT: CallDirect
// CHECK-COUNT-2: BranchArrayWithRealmArrayPrototypeAndNoNamedProperties
