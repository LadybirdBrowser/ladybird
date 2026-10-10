// With inlining off, calls of small functions stay calls.
// OPTIONS: inline-depth=0

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
// CHECK-COUNT-2: CallDirect
// CHECK-NOT: Int32Mul
