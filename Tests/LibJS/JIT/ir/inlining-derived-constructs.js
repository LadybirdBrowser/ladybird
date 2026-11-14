// REQUIRES: inline-allocation
// A construct of a derived constructor without fields inlines the derived constructor and the construct of its base
// parent at super(), which creates `this` from the "prototype" of the derived constructor. Default derived
// constructors pass their arguments on the same way.

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
class C extends A {}
function make(i) {
    return new B(i).x;
}
function makeDefault(i) {
    return new C(i).x;
}
jit.prepare(make);
jit.prepare(makeDefault);
for (let i = 0; i < 10; ++i) {
    make(i);
    makeDefault(i);
}
jit.compile(make);
jit.compile(makeDefault);

// CHECK-FUNCTION: make
// CHECK-PASS: graph building
// CHECK-NOT: CallConstruct
// CHECK-NOT: SuperCall
// CHECK: AllocateObject
// CHECK-NOT: CallConstruct
// CHECK-NOT: SuperCall
// CHECK-NOT: BindThisFromSuperCall

// CHECK-FUNCTION: makeDefault
// CHECK-PASS: graph building
// CHECK-NOT: CallConstruct
// CHECK-NOT: SuperCall
// CHECK: AllocateObject
// CHECK-NOT: CallConstruct
// CHECK-NOT: SuperCall
// CHECK-NOT: BindThisFromSuperCall
