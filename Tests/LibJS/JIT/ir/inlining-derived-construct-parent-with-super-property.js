// REQUIRES: inline-allocation
// The super() of an inlined derived construct inlines a parent constructor that accesses a super property, which
// the check made before the derived construct is inlined allows like the inlining of the super() does.

class A {
    constructor(x) {
        this.x = x;
        this.inherited = super.toString === Object.prototype.toString;
    }
}
class B extends A {
    constructor(x) {
        super(x);
    }
}
function make(i) {
    return new B(i).x;
}
jit.prepare(make);
for (let i = 0; i < 10; ++i) make(i);
jit.compile(make);

// CHECK-FUNCTION: make
// CHECK-PASS: graph building
// CHECK-NOT: CallConstruct
// CHECK-NOT: SuperCall
// CHECK: AllocateObject
// CHECK-NOT: CallConstruct
// CHECK-NOT: SuperCall
