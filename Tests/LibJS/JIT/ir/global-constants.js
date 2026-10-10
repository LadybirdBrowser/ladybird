// A property of the global object that holds an object, such as a function a script declares or a `var` initializes,
// is a constant while it is never assigned, on which the code depends: calling it needs no lookup. One that was
// assigned is loaded from the global object and checked for the object it held.

function add(a, b) {
    return a + b;
}
var subtract = function (a, b) {
    return a - b;
};
function replaced(a) {
    return a;
}
replaced = function (a) {
    return a + 1;
};

function sum(n) {
    let s = 0;
    for (let i = 0; i < n; ++i) s = add(i, s) + subtract(1, 1) + replaced(0);
    return s;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum(10);
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: graph building
// CHECK-COUNT-1: CheckShape
// CHECK-COUNT-1: LoadNamed
// CHECK-COUNT-1: CheckValue
