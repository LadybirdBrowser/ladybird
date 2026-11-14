// REQUIRES: inline-allocation

// An inlined callee creates closures like the compiled function does, and calls of them inline too, so a closure
// that nothing but frames of inlined calls refer to is never allocated.

function apply(f, x) {
    return f(x);
}
function doubled(x) {
    return apply(y => y * 2, x);
}
function sum(n) {
    let total = 0;
    for (let i = 0; i < n; i++) total += doubled(i);
    return total;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum(10);
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: graph building
// CHECK-NOT: CallSlowPath NewFunction
// CHECK: AllocateFunction e1:
// CHECK-NOT: CallSlowPath Call
// CHECK: Int32Mul
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateFunction
// CHECK: Int32Mul
