// REQUIRES: inline-allocation
// OPTIONS: inline-depth=2

// A closure that nothing but the frames of inlined calls of it refers to is never allocated.

function apply(f, x) {
    return f(x);
}

function doubled(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) sum += apply(x => x * 2, i);
    return sum;
}
jit.prepare(doubled);
jit.prepare(apply);
for (let i = 0; i < 10; ++i) doubled(10);
jit.compile(doubled);

// CHECK-FUNCTION: doubled
// CHECK-PASS: graph building
// CHECK: AllocateFunction
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateFunction
// CHECK: Int32Mul

// A closure that code reads as a value is allocated.

function returned(n) {
    const f = x => x + n;
    apply(f, n);
    return f;
}
jit.prepare(returned);
for (let i = 0; i < 10; ++i) returned(i);
jit.compile(returned);

// CHECK-FUNCTION: returned
// CHECK-PASS: escape analysis
// CHECK: [[CLOSURE:v[0-9]+]] = AllocateFunction
// CHECK: Return ([[CLOSURE]])

// A closure of a sloppy function, arrow functions included, which shows its frames as the legacy caller of the
// functions they call, is allocated when an inlined call of it calls a function without the frames of inlined calls.

function callee() {
    return 1;
}
function sloppyCaller(n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += apply(function (x) {
            return callee() + x;
        }, i);
    return sum;
}
function sloppyArrowCaller(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) sum += apply(x => callee() + x, i);
    return sum;
}
function strictCaller(n) {
    "use strict";
    let sum = 0;
    for (let i = 0; i < n; i++) sum += apply(x => callee() + x, i);
    return sum;
}
jit.prepare(sloppyCaller);
jit.prepare(sloppyArrowCaller);
jit.prepare(strictCaller);
for (let i = 0; i < 10; ++i) {
    sloppyCaller(10);
    sloppyArrowCaller(10);
    strictCaller(10);
}
jit.compile(sloppyCaller);
jit.compile(sloppyArrowCaller);
jit.compile(strictCaller);

// CHECK-FUNCTION: sloppyCaller
// CHECK-PASS: escape analysis
// CHECK: AllocateFunction

// CHECK-FUNCTION: sloppyArrowCaller
// CHECK-PASS: escape analysis
// CHECK: AllocateFunction

// CHECK-FUNCTION: strictCaller
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateFunction
