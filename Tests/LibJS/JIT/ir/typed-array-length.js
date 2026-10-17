// The length of a typed array is the length it caches, behind checks of the
// getter of %TypedArray%.prototype.length and of the typed array.

function lengthOf(array) {
    return array.length;
}
const array = new Uint16Array(5);
jit.prepare(lengthOf);
for (let i = 0; i < 10; ++i) lengthOf(array);
jit.compile(lengthOf);
if (lengthOf(array) !== 5) throw new Error("wrong length");
if (lengthOf(new Uint16Array(0)) !== 0) throw new Error("wrong length of an empty typed array");
const detached = new Uint16Array(new ArrayBuffer(8));
detached.buffer.transfer();
if (lengthOf(detached) !== 0) throw new Error("wrong length of a detached typed array");

// Typed arrays of several kinds reach the getter through their own prototype chains.
function polymorphicLengthOf(array) {
    return array.length;
}
jit.prepare(polymorphicLengthOf);
for (let i = 0; i < 10; ++i) {
    polymorphicLengthOf(new Uint8Array(3));
    polymorphicLengthOf(new Float64Array(4));
}
jit.compile(polymorphicLengthOf);
if (polymorphicLengthOf(new Float64Array(2)) !== 2) throw new Error("wrong length");

// CHECK-FUNCTION: lengthOf
// CHECK-PASS: frame initialization
// CHECK: CheckAccessorFunction
// CHECK-NEXT: CheckTypedArray
// CHECK-NEXT: LoadTypedArrayLength
// CHECK-NEXT: BranchInt32GreaterThan

// CHECK-FUNCTION: polymorphicLengthOf
// CHECK-PASS: frame initialization
// CHECK-NOT: ProbePropertyCache
// CHECK: CheckAccessorFunction
// CHECK-NEXT: CheckTypedArray
// CHECK-NEXT: LoadTypedArrayLength

// The rare cases of the length (an empty, detached or out-of-bounds typed array) exit instead of running the slow
// path, so a loop bounded by the length runs no code that could change it, and its checks move out of the loop.
function countEqual(left, right) {
    let count = 0;
    for (let i = 0; i < left.length; ++i) {
        if (left[i] === right[i]) ++count;
    }
    return count;
}
const left = new Uint8Array([1, 2, 3, 4]);
const right = new Uint8Array([1, 0, 3, 0]);
jit.prepare(countEqual);
for (let i = 0; i < 10; ++i) countEqual(left, right);
jit.compile(countEqual);
if (countEqual(left, right) !== 2) throw new Error("wrong count");
if (countEqual(new Uint8Array(0), right) !== 0) throw new Error("wrong count of an empty typed array");

// CHECK-FUNCTION: countEqual
// CHECK-PASS: graph building
// CHECK: BranchInt32GreaterThan ({{v[0-9]+}}, #i32:0) -> {{b[0-9]+}}, [[COLD:b[0-9]+]]
// CHECK: [[COLD]] <- {{b[0-9]+}} cold:
// CHECK-NEXT: Exit SlowPath
// CHECK-NOT: CallSlowPath GetLength
// CHECK-PASS: loop invariant code motion
// CHECK: CheckAccessorFunction
// CHECK: loop:
// CHECK-NOT: CheckAccessorFunction
