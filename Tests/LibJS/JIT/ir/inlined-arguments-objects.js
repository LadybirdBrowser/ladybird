// REQUIRES: inline-allocation
// Inlined callees keep their arguments objects virtual: forwarding them with apply inlines the call with the values
// the inlined call passed, and their length and constant indices are those values.

const Class = {
    create() {
        return function () {
            this.initialize.apply(this, arguments);
        };
    },
};
const Point = Class.create();
Point.prototype = {
    initialize(x, y) {
        this.x = x;
        this.y = y;
    },
};
const Color = Class.create();
Color.prototype = {
    initialize(r, g, b) {
        this.r = r;
        this.g = g;
        this.b = b;
    },
};
function second() {
    return arguments.length + arguments[1];
}
function make(i) {
    return new Point(i, 2).x + second(i, 3);
}
jit.prepare(make);
for (let i = 0; i < 10; ++i) {
    new Color(1, 2, 3);
    make(i);
}
jit.compile(make);

// CHECK-FUNCTION: make
// CHECK-PASS: graph building
// CHECK-NOT: CreateArguments
// CHECK-NOT: CallSlowPath
// CHECK-NOT: CallForwardingArguments
// CHECK: Return
