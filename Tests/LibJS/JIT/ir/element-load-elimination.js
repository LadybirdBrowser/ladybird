// Check elimination keeps one load of an element read twice with nothing in between that could change it, and the
// checks of the second load go with it. A store of any element in between makes the second load stay.

function digits(a, i) {
    return (a[i] & 0x3fff) + (a[i] >> 14);
}
jit.prepare(digits);
for (let k = 0; k < 10; ++k) digits([1 << 20, 2, 3], k % 3);
jit.compile(digits);

// CHECK-FUNCTION: digits
// CHECK-PASS: graph building
// CHECK-COUNT-2: LoadElementAt Packed
// CHECK-PASS: check elimination
// CHECK: [[ELEMENT:v[0-9]+]] = LoadElementAt Packed
// CHECK-NEXT: CheckInt32 ([[ELEMENT]])
// CHECK-NOT: LoadElementAt

// An index that is a load the analysis already replaced by an earlier load of the same property is that index.

function viaProperty(a, o) {
    return (a[o.i] & 0x3fff) + (a[o.i] >> 14);
}
jit.prepare(viaProperty);
for (let k = 0; k < 10; ++k) viaProperty([1 << 20, 2, 3], { i: k % 3 });
jit.compile(viaProperty);

// CHECK-FUNCTION: viaProperty
// CHECK-PASS: graph building
// CHECK-COUNT-2: LoadElementAt Packed
// CHECK-PASS: check elimination
// CHECK-COUNT-1: LoadElementAt Packed

function swap(a, b, i) {
    const before = a[i];
    b[i] = 0;
    return before + a[i];
}
jit.prepare(swap);
for (let k = 0; k < 10; ++k) swap([1, 2], [3, 4], k % 2);
jit.compile(swap);

// CHECK-FUNCTION: swap
// CHECK-PASS: check elimination
// CHECK: LoadElementAt Packed
// CHECK: StoreElementAt Packed
// CHECK: LoadElementAt Packed

// Every access of a typed array computes its data address, and reads of the same element share one load all the same.

function bytePair(bytes, i) {
    return (bytes[i] & 0x0f) | (bytes[i] >> 4);
}
jit.prepare(bytePair);
for (let k = 0; k < 10; ++k) bytePair(new Uint8Array([0x12, 0x34]), k % 2);
jit.compile(bytePair);

// CHECK-FUNCTION: bytePair
// CHECK-PASS: graph building
// CHECK-COUNT-2: LoadElementAt TypedArray(Uint8)
// CHECK-PASS: check elimination
// CHECK: LoadElementAt TypedArray(Uint8)
// CHECK-NOT: LoadElementAt
