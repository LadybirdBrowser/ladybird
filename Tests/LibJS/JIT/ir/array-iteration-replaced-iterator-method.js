// Once %Array.prototype%[@@iterator] was replaced, the realm's arrays may be iterated observably for good, and for-of
// loops over them take the slow path of GetIterator, which calls the iterator method.

const values = Array.prototype.values;
let iteratorMethodCalls = 0;
Array.prototype[Symbol.iterator] = function () {
    ++iteratorMethodCalls;
    return values.call(this);
};

function sum(array) {
    let total = 0;
    for (const value of array) total += value;
    return total;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum([1, 2, 3]);
jit.compile(sum);
if (sum([4, 5]) !== 9 || iteratorMethodCalls !== 11) throw new Error("wrong iteration");

// CHECK-FUNCTION: sum
// CHECK-PASS: frame initialization
// CHECK-NOT: BranchArrayWithRealmArrayPrototypeAndNoNamedProperties
// CHECK: CallSlowPath GetIterator
