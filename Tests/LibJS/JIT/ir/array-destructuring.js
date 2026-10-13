// Array destructuring iterates arrays without array iterators, and closing
// such an iteration does nothing while array iterators have no return
// method.

function sumPair(pair) {
    const [first, second] = pair;
    return first + second;
}
jit.prepare(sumPair);
for (let i = 0; i < 10; ++i) sumPair([1, 2]);
jit.compile(sumPair);
if (sumPair([3, 4]) !== 7) throw new Error("wrong sum");

// CHECK-FUNCTION: sumPair
// CHECK-PASS: graph building
// CHECK: OpenArrayIteration
// CHECK-NOT: Allocate
// CHECK: CallSlowPath IteratorNextUnpack
// CHECK-NOT: Allocate
// CHECK: CallSlowPath IteratorNextUnpack
// CHECK-NOT: Allocate
// CHECK: BranchNonNegativeInt32 ([[ITERATOR:v\d+]]) -> [[FAST:b\d+]], [[OTHER:b\d+]]
// CHECK-NEXT: [[FAST]] <- {{b\d+}}:
// CHECK-NEXT: AssumeValid
// CHECK: [[OTHER]] <- {{b\d+}}:
// CHECK-NEXT: ProbePropertyCache cache {{\d+}} ([[ITERATOR]])
// CHECK: CallSlowPath IteratorClose
