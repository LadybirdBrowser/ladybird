// Every loop checks for interrupts where it jumps back, and keeps the check in the loop. Loops that count to an end lose
// theirs if they have a loop around them to stop at, or end soon by themselves.

function untilZero(object) {
    while (object.value !== 0) object.value = object.next;
}
function counts(object, count) {
    let sum = 0;
    for (let i = 0; i < count; ++i) sum = (sum + object.value) | 0;
    return sum;
}
function wraps(object) {
    let i = 0;
    while (object.value-- > 0) i = (i + 2) | 0;
    return i;
}
function short(object) {
    let sum = 0;
    for (let i = 0; i < 4000; ++i) sum = (sum + object.value) | 0;
    return sum;
}
function notShort(object) {
    let sum = 0;
    for (let i = 0; i < 6000; ++i) sum = (sum + object.value) | 0;
    return sum;
}
function nested(count) {
    let sum = 0;
    for (let i = 0; i < count; ++i) {
        for (let j = 0; j < 10; ++j) sum = (sum + j) | 0;
    }
    return sum;
}

jit.prepare(untilZero);
jit.prepare(counts);
jit.prepare(wraps);
jit.prepare(short);
jit.prepare(notShort);
jit.prepare(nested);
for (let i = 0; i < 10; ++i) {
    untilZero({ value: 1, next: 0 });
    counts({ value: i }, 10);
    wraps({ value: 2 });
    short({ value: i });
    notShort({ value: i });
    nested(2);
}
jit.compile(untilZero);
jit.compile(counts);
jit.compile(wraps);
jit.compile(short);
jit.compile(notShort);
jit.compile(nested);

// CHECK-FUNCTION: untilZero
// CHECK-PASS: loop invariant code motion
// CHECK: loop:
// CHECK: CheckInterrupt
// CHECK-PASS: interrupt check elimination
// CHECK: CheckInterrupt

// CHECK-FUNCTION: counts
// CHECK-PASS: interrupt check elimination
// CHECK: CheckInterrupt

// CHECK-FUNCTION: short
// CHECK-PASS: graph building
// CHECK: CheckInterrupt
// CHECK-PASS: interrupt check elimination
// CHECK-COUNT-0: CheckInterrupt

// CHECK-FUNCTION: notShort
// CHECK-PASS: interrupt check elimination
// CHECK: CheckInterrupt

// CHECK-FUNCTION: wraps
// CHECK-PASS: interrupt check elimination
// CHECK: CheckInterrupt

// CHECK-FUNCTION: nested
// CHECK-PASS: graph building
// CHECK-COUNT-2: CheckInterrupt
// CHECK-PASS: interrupt check elimination
// CHECK-COUNT-1: CheckInterrupt
