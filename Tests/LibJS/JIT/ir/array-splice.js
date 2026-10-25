function spliceThree(array, start, count, x, y, z) {
    return array.splice(start, count, x, y, z);
}
jit.prepare(spliceThree);
for (let i = 0; i < 10; ++i) spliceThree([1, 2, 3, 4], 1, 1, 5, 6, 7);
jit.compile(spliceThree);

// CHECK-FUNCTION: spliceThree
// CHECK-PASS: graph building
// CHECK: CallArraySplice
// CHECK-NOT: CallNative
// CHECK: CallSlowPath Call

function deleteOnly(array, start, count) {
    return array.splice(start, count);
}
jit.prepare(deleteOnly);
for (let i = 0; i < 10; ++i) deleteOnly([1, 2, 3, 4], 1, 2);
jit.compile(deleteOnly);

// CHECK-FUNCTION: deleteOnly
// CHECK-PASS: graph building
// CHECK: CallArraySplice
// CHECK-NOT: CallNative
// CHECK: CallSlowPath Call
