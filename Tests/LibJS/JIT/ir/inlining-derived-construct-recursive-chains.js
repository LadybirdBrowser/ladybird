// REQUIRES: inline-allocation
// The classes of a chain made from one class expression share their code, and only so many frames of the same code
// are inlined in one another. A construct of a chain longer than that is not inlined, and its caller still compiles.

const mixin = Base =>
    class extends Base {
        constructor(x) {
            super(x);
        }
    };
class A {
    constructor(x) {
        this.x = x;
    }
}
const B = mixin(mixin(mixin(mixin(A))));
function make(i) {
    return new B(i).x;
}
jit.prepare(make);
for (let i = 0; i < 10; ++i) make(i);
jit.compile(make);

// CHECK-FUNCTION: make
// CHECK-PASS: graph building
// CHECK: CallConstruct
// CHECK-NOT: AllocateObject
