// Calls of small functions with one target are inlined; with inlining off they stay calls.

function square(x) {
    return x * x;
}
function sumOfSquares(a, b) {
    return square(a) + square(b);
}
jit.prepare(square);
jit.prepare(sumOfSquares);
for (let i = 0; i < 10; ++i) sumOfSquares(i, i + 1);
jit.compile(sumOfSquares);

// CHECK-FUNCTION: sumOfSquares
// CHECK-PASS: frame initialization
// CHECK-NOT: CallDirect
// CHECK-COUNT-2: Int32Mul
// CHECK: fs{{[0-9]+}}: e1 pc {{[0-9]+}} at {{.*}} in fs{{[0-9]+}}

// An inlined callee returning a value that is never empty, here an int32 value, returns it as it is.

function first(a, b) {
    return a;
}
function loopFirst(n) {
    let s = 0;
    for (let i = 0; i < n; ++i) s = first(i, s);
    return s;
}
jit.prepare(first);
jit.prepare(loopFirst);
for (let i = 0; i < 10; ++i) loopFirst(5);
jit.compile(loopFirst);

// CHECK-FUNCTION: loopFirst
// CHECK-PASS: dead code elimination
// CHECK-NOT: CallDirect
// CHECK-NOT: EmptyToUndefined
