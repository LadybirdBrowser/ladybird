// REQUIRES: inline-allocation
// The graph builder knows the shape of an object it allocated, so a method call on it reads the method of that shape
// as a constant and inlines the call, though the call site saw several methods of several shapes.

function A() {}
A.prototype.value = function () {
    return 1;
};
function B() {}
B.prototype.value = function () {
    return 2;
};
function invoke(object) {
    return object.value();
}
function make() {
    return invoke(new A());
}
jit.prepare(make);
for (let i = 0; i < 10; ++i) {
    invoke(new B());
    make();
}
jit.compile(make);

// CHECK-FUNCTION: make
// CHECK-PASS: graph building
// CHECK-NOT: ShapeSwitch
// CHECK-NOT: CallSlowPath
// CHECK: Return
