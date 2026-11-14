// OPTIONS: inline-depth=1

// A direct call in an inlined callee that runs without the frames of the inlined calls checks its callee first and
// exits for another one, rather than having the runtime create those frames to call it, every time.

function target(x) {
    return x + 1;
}
function apply(f, x) {
    return f(x);
}
function caller(f, n) {
    let sum = 0;
    for (let i = 0; i < n; i++) sum += apply(f, i);
    return sum;
}
jit.prepare(caller);
jit.prepare(apply);
for (let i = 0; i < 10; ++i) caller(target, 10);
jit.compile(caller);

// CHECK-FUNCTION: caller
// CHECK-PASS: graph building
// CHECK: CheckValue
// CHECK-NEXT: CallDirect @e1:{{[0-9]+}} without inlined frames
