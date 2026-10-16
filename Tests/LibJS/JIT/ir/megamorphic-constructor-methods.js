// REQUIRES: inline-allocation
// A constructor that many classes share reads their methods through a megamorphic cache. Inlined into a construct,
// whose object has a known shape, it reads the method of that shape as a constant and inlines its call.

function makeClass(scale) {
    const constructor = function (value) {
        this.initialize(value);
    };
    constructor.prototype = {
        initialize(value) {
            this.value = value * scale;
        },
    };
    return constructor;
}
const classes = [];
for (let scale = 1; scale <= 8; ++scale) classes.push(makeClass(scale));
const First = classes[0];
function make(i) {
    return new First(i).value;
}
jit.prepare(make);
for (let i = 0; i < 10; ++i) {
    for (const Class of classes) new Class(i);
    make(i);
}
jit.compile(make);

// CHECK-FUNCTION: make
// CHECK-PASS: graph building
// CHECK-NOT: CallSlowPath GetById
// CHECK-NOT: CallSlowPath Call @
// CHECK: Return
