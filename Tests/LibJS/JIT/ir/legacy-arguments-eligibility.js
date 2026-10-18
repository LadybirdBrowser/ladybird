// Calls in inlined functions with legacy arguments properties may run code that reads the function's arguments through
// them. Objects among them that the code never allocated stay unallocated: the runtime creates them for code that reads
// them, and the code exits after the call (AssumeValid). Where code read them before, recompiles allocate them there.

function opaque(read) {
    // A debugger statement keeps this function from being inlined.
    debugger;
    if (read) return ordinary.arguments[0].x;
}
const arrow = object => {
    opaque(false);
    return object.x;
};
const holder = {
    method(object) {
        opaque(false);
        return object.x;
    },
};
function ordinary(object, read) {
    opaque(read);
    return object.x;
}
function throughArrow(x) {
    return arrow({ x });
}
function throughMethod(x) {
    return holder.method({ x });
}
function throughOrdinary(x) {
    return ordinary({ x }, false);
}
function throughOrdinaryAfterRead(x, read) {
    return ordinary({ x }, read);
}
for (const fn of [
    opaque,
    arrow,
    holder.method,
    ordinary,
    throughArrow,
    throughMethod,
    throughOrdinary,
    throughOrdinaryAfterRead,
])
    jit.prepare(fn);
for (let i = 0; i < 10; ++i) {
    throughArrow(i);
    throughMethod(i);
    throughOrdinary(i);
    throughOrdinaryAfterRead(i, false);
}
jit.compile(throughArrow);
jit.compile(throughMethod);
jit.compile(throughOrdinary);
jit.compile(throughOrdinaryAfterRead);
throughOrdinaryAfterRead(1, true);
jit.compile(throughOrdinaryAfterRead);

// CHECK-FUNCTION: throughArrow
// CHECK-PASS: graph building
// CHECK: AllocateObject
// CHECK-NOT: AssumeLegacyArgumentsUnread
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateObject

// CHECK-FUNCTION: throughMethod
// CHECK-PASS: graph building
// CHECK: AllocateObject
// CHECK-NOT: AssumeLegacyArgumentsUnread
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateObject

// CHECK-FUNCTION: throughOrdinary
// CHECK-PASS: graph building
// CHECK: AllocateObject
// CHECK: CallDirect
// CHECK-NEXT: AssumeLegacyArgumentsUnread
// CHECK-PASS: escape analysis
// CHECK-NOT: AllocateObject
// CHECK: CallDirect
// CHECK-NEXT: AssumeValid

// CHECK-FUNCTION: throughOrdinaryAfterRead
// CHECK-PASS: escape analysis
// CHECK: AllocateObject
// CHECK: CallDirect
