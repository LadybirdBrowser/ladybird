// REQUIRES: inline-allocation
// A template literal concatenates its strings with the strings of integers the VM has inline, and leaves the rest to
// the slow path, which makes a string too: the length of the result is that of a string, whichever path made it.

function describe(value, count) {
    return `value ${value} of ${count}`.length;
}
jit.prepare(describe);
for (let i = 0; i < 10; ++i) describe(i, 100000);
jit.compile(describe);

// CHECK-FUNCTION: describe
// CHECK-PASS: dead code elimination
// CHECK-COUNT-2: = IntegerToString
// CHECK-COUNT-3: = ConcatenateStrings
// CHECK-COUNT-3: = CallSlowPath ConcatString
// CHECK-COUNT-1: = StringLength
// CHECK-COUNT-0: BranchObject

// The slow path may call a toString() that changes any object, so a property loaded before it is loaded again after.
function reread(o, s) {
    const text = `${s}${o.x}${o.value}`;
    return o.x + text.length;
}
const o = { x: 1, value: 5 };
jit.prepare(reread);
for (let i = 0; i < 10; ++i) reread(o, "s");
jit.compile(reread);

// CHECK-FUNCTION: reread
// CHECK-PASS: dead code elimination
// CHECK-COUNT-2: = LoadNamed +0
