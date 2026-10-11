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

// CHECK-FUNCTION: lengthOf
// CHECK-PASS: frame initialization
// CHECK: CheckAccessorFunction
// CHECK-NEXT: CheckTypedArray
// CHECK-NEXT: LoadTypedArrayLength
// CHECK-NEXT: BranchInt32GreaterThan
