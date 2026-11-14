// REQUIRES: inline-allocation
// A construct of a base constructor whose fields have literal values creates `this` with the shape it has after the
// fields and stores their values. A derived constructor with literal fields adds them to `this` once it checked the
// shape they start from. One with private methods initializes its instance elements through the slow path of
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
class C extends A {
    #m() {}
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
function makeDerivedWithPrivateMethod(i) {
    return new C(i).x;
}
jit.prepare(makeBase);
jit.prepare(makeDerived);
jit.prepare(makeDerivedWithPrivateMethod);
for (let i = 0; i < 10; ++i) {
    makeBase(i);
    makeDerived(i);
    makeDerivedWithPrivateMethod(i);
}
jit.compile(makeBase);
jit.compile(makeDerived);
jit.compile(makeDerivedWithPrivateMethod);

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
// CHECK: CheckShape
// CHECK: AddNamed
// CHECK-NOT: BindThisFromSuperCall
// CHECK-NOT: CallConstruct

// CHECK-FUNCTION: makeDerivedWithPrivateMethod
// CHECK-PASS: graph building
// CHECK-NOT: CallConstruct
// CHECK: AllocateObject
// CHECK: CallSlowPath BindThisFromSuperCall
// CHECK-NOT: CallConstruct
