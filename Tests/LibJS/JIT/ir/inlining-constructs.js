// REQUIRES: inline-allocation
// OPTIONS: inline-budget=0
// Constructors of up to 50 instructions are inlined into their constructs whatever the inlining budget says: here
// it allows no other inlining.

function Node() {
    this.p0 = 0;
    this.p1 = 1;
    this.p2 = 2;
    this.p3 = 3;
    this.p4 = 4;
    this.p5 = 5;
    this.p6 = 6;
    this.p7 = 7;
    this.p8 = 8;
    this.p9 = 9;
    this.p10 = 10;
    this.p11 = 11;
    this.p12 = 12;
    this.p13 = 13;
    this.p14 = 14;
    this.p15 = 15;
    this.p16 = 16;
    this.p17 = 17;
    this.p18 = 18;
    this.p19 = 19;
}
function make() {
    return new Node();
}
jit.prepare(make);
for (let i = 0; i < 10; ++i) make();
jit.compile(make);

// CHECK-FUNCTION: make
// CHECK-PASS: graph building
// CHECK-NOT: CallConstruct
// CHECK: AddNamed
