// REQUIRES: inline-allocation
//
// The multiplication's check of value is the last user of the frame state that refers to the object escape analysis
// removed, and so of the phi of its property. Only the value numbering after loop invariant code motion merges it with
// the check hoisted out of the loop, which leaves the phi unused. It must not stay in a register across the call.

function phiOfVirtualObjectProperty(flag, value) {
    let number = 1;
    for (let i = 0; i < 4; ++i) number = value;
    const object = { a: 1 };
    if (flag) object.a = number;
    object.b = value * number;
    return flag / flag;
}
jit.prepare(phiOfVirtualObjectProperty);
for (let i = 0; i < 10; ++i) phiOfVirtualObjectProperty(i & 1 ? {} : null, 1.5);
jit.compile(phiOfVirtualObjectProperty);

// CHECK-FUNCTION: phiOfVirtualObjectProperty
// CHECK-PASS: branch fusion
// CHECK: = Phi (#1, #1,
// CHECK-PASS: refinement removal
// CHECK-NOT: = Phi (#1, #1,
