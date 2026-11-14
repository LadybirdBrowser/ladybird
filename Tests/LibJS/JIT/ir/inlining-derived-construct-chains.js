// REQUIRES: inline-allocation
// A construct of a derived constructor whose parents are derived constructors without fields too inlines each level's
// construct at its super(), down to the base constructor, which creates `this`.

class A {
    constructor(x) {
        this.x = x;
    }
}
class B extends A {
    constructor(x) {
        super(x);
        this.y = 1;
    }
}
class C extends B {}
class D extends C {
    constructor(x) {
        super(x);
    }
}
function make(i) {
    return new D(i).x;
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
// CHECK-NOT: BindThisFromSuperCall
