// REQUIRES: inline-allocation
// A construct of a base constructor whose fields have literal values creates `this` with the shape it has after the
// fields and stores their values. A derived constructor with fields initializes them through the slow path of
// BindThisFromSuperCall, in the published frames of the inlined construct.

class A {
    a = 1;
    b = 2;
    constructor(x) {
        this.x = x;
    }
}
class B extends A {
    c = 3;
    constructor(x) {
        super(x);
    }
}
function makeBase(i) {
    return new A(i).x;
}
function makeDerived(i) {
    return new B(i).x;
}
jit.prepare(makeBase);
jit.prepare(makeDerived);
for (let i = 0; i < 10; ++i) {
    makeBase(i);
    makeDerived(i);
}
jit.compile(makeBase);
jit.compile(makeDerived);

// CHECK-FUNCTION: makeBase
// CHECK-PASS: graph building
// CHECK-NOT: CallConstruct
// CHECK: AllocateObject
// CHECK: InitializeNamed
// CHECK: InitializeNamed
// CHECK-NOT: CallConstruct

// CHECK-FUNCTION: makeDerived
// CHECK-PASS: graph building
// CHECK-NOT: CallConstruct
// CHECK-NOT: SuperCall
// CHECK: AllocateObject
// CHECK: InitializeNamed
// CHECK: CallSlowPath BindThisFromSuperCall
// CHECK-NOT: CallConstruct
