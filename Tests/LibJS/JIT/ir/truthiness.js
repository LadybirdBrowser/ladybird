// The truthiness of an int32 value compares it with 0, without boxing it.

function elementOr(data, i) {
    return data[i] || 65536;
}
const data = new Uint16Array([0, 1, 2]);
jit.prepare(elementOr);
for (let i = 0; i < 10; ++i) elementOr(data, i % 3);
jit.compile(elementOr);
if (elementOr(data, 0) !== 65536 || elementOr(data, 2) !== 2) throw new Error("wrong truthiness");

// CHECK-FUNCTION: elementOr
// CHECK-PASS: frame initialization
// CHECK: [[ELEMENT:v\d+]]:int32 = LoadElementAt TypedArray(Uint16)
// CHECK-NOT: BranchTruthy
// CHECK: BranchInt32StrictlyInequals ([[ELEMENT]], #i32:0)
