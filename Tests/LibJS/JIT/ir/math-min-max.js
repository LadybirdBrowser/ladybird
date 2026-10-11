// Math.min and Math.max are pure value nodes: of int32 values where their
// results were int32 values, and of numbers otherwise, the code exiting for
// other arguments. Value numbering shares them, and loops hoist them like other
// pure nodes.

function clamp(value, low, high) {
    return Math.max(low, Math.min(value, high));
}
jit.prepare(clamp);
for (let i = 0; i < 10; ++i) clamp(i, 2, 7);
jit.compile(clamp);
if (clamp(10, 2, 7) !== 7 || clamp(-3, 2, 7) !== 2 || clamp(5, 2, 7) !== 5) throw new Error("wrong clamp");

// CHECK-FUNCTION: clamp
// CHECK-PASS: frame initialization
// CHECK-NOT: CallNative
// CHECK: [[VALUE:v[0-9]+]]:int32 = CheckInt32
// CHECK-NEXT: [[HIGH:v[0-9]+]]:int32 = CheckInt32
// CHECK-NEXT: [[MIN:v[0-9]+]]:int32 = Int32Min ([[VALUE]], [[HIGH]])
// CHECK: [[LOW:v[0-9]+]]:int32 = CheckInt32
// CHECK-NEXT: Int32Max ([[LOW]], [[MIN]])
// CHECK-NOT: Branch

// Constant arguments need no check, and constants of other kinds make the
// call compute with doubles.
function clampLow(value) {
    return Math.max(value, 0);
}
function clampHigh(value) {
    return Math.min(value, 2.5);
}
jit.prepare(clampLow);
jit.prepare(clampHigh);
for (let i = 0; i < 10; ++i) {
    clampLow(i);
    clampHigh(1);
}
jit.compile(clampLow);
jit.compile(clampHigh);
if (clampLow(-3) !== 0 || clampHigh(3) !== 2.5) throw new Error("wrong clamp");

// CHECK-FUNCTION: clampLow
// CHECK-PASS: frame initialization
// CHECK-NOT: CallNative
// CHECK-COUNT-1: CheckInt32
// CHECK: Int32Max ({{v[0-9]+}}, #i32:0)

// CHECK-FUNCTION: clampHigh
// CHECK-PASS: frame initialization
// CHECK-NOT: CheckInt32
// CHECK-NOT: CallNative
// CHECK: Float64Min ({{v[0-9]+}}, #0x4004000000000000)

function smallest(a, b, c) {
    return Math.min(a, b, c);
}
jit.prepare(smallest);
for (let i = 0; i < 10; ++i) smallest(i + 0.5, 2, -1.25);
jit.compile(smallest);
if (!Number.isNaN(smallest(1, NaN, 2)) || !Object.is(smallest(0, -0, 0), -0)) throw new Error("wrong min");

// CHECK-FUNCTION: smallest
// CHECK-PASS: graph building
// CHECK-COUNT-3: CheckNumber
// CHECK: [[FIRST:v[0-9]+]]:float64 = Float64Min
// CHECK-NEXT: [[SMALLEST:v[0-9]+]]:float64 = Float64Min ([[FIRST]], {{v[0-9]+}})
// CHECK-NEXT: BoxFloat64 ([[SMALLEST]])
// CHECK-NOT: CallSlowPath

function twice(a, b) {
    return Math.max(a, b) + Math.max(a, b);
}
jit.prepare(twice);
for (let i = 0; i < 10; ++i) twice(i, 3);
jit.compile(twice);

// CHECK-FUNCTION: twice
// CHECK-PASS: global value numbering
// CHECK-COUNT-1: Int32Max

function bounded(values, limit, bound) {
    let total = 0;
    for (let i = 0; i < values.length; ++i) total = (total + Math.min(values[i], Math.max(limit, bound))) | 0;
    return total;
}
jit.prepare(bounded);
for (let i = 0; i < 10; ++i) bounded([1, 5, 9], 4, 2);
jit.compile(bounded);

// CHECK-FUNCTION: bounded
// CHECK-PASS: loop invariant code motion
// CHECK: Int32Max
// CHECK: loop:
// CHECK-NOT: Int32Max
// CHECK: Int32Min

console.log(clamp(3, 0, 2), smallest(3, 1.5, 2), twice(1, 2), bounded([1, 2, 3], 1, 0));
