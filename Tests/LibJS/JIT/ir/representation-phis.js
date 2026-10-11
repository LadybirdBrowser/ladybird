// Representation selection unboxes phis: a phi of int32 values whose other input is a tagged value from before a loop
// checks that value where it enters the loop, where the loop checks the phi on every iteration anyway, and a phi of
// doubles and int32 values joins them as doubles.

function sum(start, count) {
    let total = start;
    for (let i = 0; i < count; ++i) total = total + i;
    return total;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum(i, 10);
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: representation selection
// CHECK: [[START:v[0-9]+]] = LoadSlot a0
// CHECK: [[CHECKED:v[0-9]+]]:int32 = CheckInt32 ([[START]])
// CHECK-NEXT: Jump
// CHECK: loop:
// CHECK: [[TOTAL:v[0-9]+]]:int32 = Phi ([[CHECKED]], {{v[0-9]+}})
// CHECK-NOT: CheckInt32 ([[TOTAL]])

// The same holds for merges whose block always jumps on to the check, here past an inlined call.

function nothing() {}
function pickAfterCall(flag, a) {
    let x = flag ? a : 1;
    nothing();
    return x + 1;
}
jit.prepare(nothing);
jit.prepare(pickAfterCall);
for (let i = 0; i < 10; ++i) {
    pickAfterCall(true, i);
    pickAfterCall(false, i);
}
jit.compile(pickAfterCall);

// CHECK-FUNCTION: pickAfterCall
// CHECK-PASS: representation selection
// CHECK: [[A:v[0-9]+]] = LoadSlot a1
// CHECK-NEXT: [[CHECKED:v[0-9]+]]:int32 = CheckInt32 ([[A]])
// CHECK-NEXT: Jump
// CHECK: [[X:v[0-9]+]]:int32 = Phi ([[CHECKED]], #i32:1)
// CHECK-NOT: CheckInt32 ([[X]])

function scale(values, factor) {
    let total = 0;
    for (let i = 0; i < values.length; ++i) total = total + values[i] * factor;
    return total;
}
jit.prepare(scale);
for (let i = 0; i < 10; ++i) {
    scale([1, 2, 3], 0.5);
    scale([1, 2, 3], 2);
}
jit.compile(scale);

// CHECK-FUNCTION: scale
// CHECK-PASS: dead code elimination
// CHECK: loop:
// CHECK: [[TOTAL:v[0-9]+]]:float64 = Phi ({{[^)]*}})
// CHECK: Float64Add ([[TOTAL]], {{v[0-9]+}})

// Loop variables that start as uint32 typed array elements (doubles, since they may not fit an int32) are checked to
// be int32 values where they enter the loop, also where no instruction at the loop header has a frame state of its own.

const states = new Uint32Array(60);
const data = new Uint16Array(1000);
for (let s = 0; s < 10; ++s) states[s * 6 + 1] = s * 30;
for (let k = 0; k < 1000; ++k) data[k] = k % 7;
for (let s = 0; s < 10; ++s) data[s * 30 + 27] = 65535;
function stateSlot(s, slot) {
    return states[s * 6 + slot];
}
function addActions(state, token) {
    let index = 0;
    for (let i = stateSlot(state, 1); ; i += 3) {
        if (data[i] == 65535) break;
        if (data[i] == token) index++;
    }
    return index;
}
jit.prepare(stateSlot);
jit.prepare(addActions);
for (let k = 0; k < 20; ++k) addActions(k % 10, 3);
jit.compile(addActions);

// CHECK-FUNCTION: addActions
// CHECK-PASS: dead code elimination
// CHECK: [[ELEMENT:v[0-9]+]]:float64 = LoadElementAt TypedArray(Uint32)
// CHECK-NEXT: [[BOXED:v[0-9]+]] = BoxFloat64 ([[ELEMENT]])
// CHECK: [[START:v[0-9]+]]:int32 = CheckInt32 ([[BOXED]])
// CHECK: loop:
// CHECK-NEXT: [[I:v[0-9]+]]:int32 = Phi ([[START]], {{v[0-9]+}})
// CHECK-NOT: CheckInt32 ({{v[0-9]+}})
// CHECK: CheckBounds ([[I]], {{[^)]+}})
