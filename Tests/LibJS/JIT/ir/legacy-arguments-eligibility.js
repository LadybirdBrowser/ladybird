// Only functions with legacy arguments properties expose their inlined arguments to calls.

function opaque() {
    // A for-in loop keeps this function from being inlined.
    for (const key in {}) {
    }
}
const arrow = object => {
    opaque();
    return object.x;
};
const holder = {
    method(object) {
        opaque();
        return object.x;
    },
};
function ordinary(object) {
    opaque();
    return object.x;
}
function throughArrow(x) {
    return arrow({ x });
}
function throughMethod(x) {
    return holder.method({ x });
}
function throughOrdinary(x) {
    return ordinary({ x });
}
for (const fn of [opaque, arrow, holder.method, ordinary, throughArrow, throughMethod, throughOrdinary])
    jit.prepare(fn);
for (let i = 0; i < 10; ++i) {
    throughArrow(i);
    throughMethod(i);
    throughOrdinary(i);
}
jit.compile(throughArrow);
jit.compile(throughMethod);
jit.compile(throughOrdinary);

// CHECK-FUNCTION: throughArrow
// CHECK-PASS: graph building
// CHECK: AllocateObject
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateObject

// CHECK-FUNCTION: throughMethod
// CHECK-PASS: graph building
// CHECK: AllocateObject
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateObject

// CHECK-FUNCTION: throughOrdinary
// CHECK-PASS: escape analysis
// CHECK: AllocateObject
