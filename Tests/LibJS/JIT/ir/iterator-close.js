// Closing an iterator probes the cache of the site for its return method,
// and runs the slow path only if the iterator has one.

function firstTwo(pair) {
    const [first, second] = pair;
    return first + second;
}
jit.prepare(firstTwo);
for (let i = 0; i < 10; ++i) firstTwo([1, 2, 3]);
jit.compile(firstTwo);
if (firstTwo([3, 4, 5]) !== 7) throw new Error("wrong sum");

// CHECK-FUNCTION: firstTwo
// CHECK-PASS: graph building
// CHECK: [[RETURN:v\d+]] = ProbePropertyCache cache {{\d+}} ([[ITERATOR:v\d+]])
// CHECK-NEXT: BranchTaggedEquals ([[RETURN]], #empty)
// CHECK: [[REFINED:v\d+]] = RefineNotTaggedEquals ([[RETURN]], #empty)
// CHECK-NEXT: BranchNullish ([[REFINED]])
// CHECK: CallSlowPath IteratorClose {{@\d+}} ([[ITERATOR]],
