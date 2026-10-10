// Arithmetic whose feedback saw more than int32 values takes one path per kind of operand: int32 values (with int32
// operations that exit where results do not fit), then numbers as doubles. Other operands go to one slow path, whose
// result is a number too where the feedback only saw numbers (else the code exits after the instruction), so the
// paths join as a double that the next operation takes as it is.

function mixed(a, b) {
    return a * b + 0.5;
}
jit.prepare(mixed);
for (let i = 0; i < 10; ++i) {
    mixed(i, 2.5);
    mixed(i, 3);
}
jit.compile(mixed);

// CHECK-FUNCTION: mixed
// CHECK-PASS: dead code elimination
// CHECK: BranchInt32Value ([[A:v[0-9]+]])
// CHECK: Int32Mul
// CHECK: BranchDouble ([[A]])
// CHECK: Float64Mul
// CHECK: cold:
// CHECK-NEXT: CallSlowPath Mul
// CHECK-NEXT: {{v[0-9]+}}:float64 = CheckNumber ({{v[0-9]+}})
// CHECK: [[PRODUCT:v[0-9]+]]:float64 = Phi ({{v[0-9]+}}, {{v[0-9]+}}, {{v[0-9]+}})
// CHECK-NOT: Branch
// CHECK: [[SUM:v[0-9]+]]:float64 = Float64Add ([[PRODUCT]], #0x3fe0000000000000)
// CHECK-NEXT: BoxFloat64 ([[SUM]])
// CHECK-NOT: CallSlowPath Add

// Bitwise operations convert doubles with ToInt32. An unsigned shift whose results did not fit an int32 makes a double.

function bits(a, b) {
    return ((a | b) ^ ~a) >>> 0;
}
jit.prepare(bits);
for (let i = 0; i < 10; ++i) {
    bits(i * 1e9, 0.5);
    bits(-i, 3);
}
jit.compile(bits);

// CHECK-FUNCTION: bits
// CHECK-PASS: dead code elimination
// CHECK: [[DOUBLE:v[0-9]+]]:float64 = UnboxDouble
// CHECK-NEXT: Float64ToInt32 ([[DOUBLE]])
// CHECK: Int32BitwiseOr
// CHECK: Int32BitwiseXor ({{v[0-9]+}}, #i32:-1)
// CHECK: [[BITS:v[0-9]+]]:int32 = Uint32ShiftRight ({{v[0-9]+}}, #i32:0)
// CHECK-NEXT: [[UNSIGNED:v[0-9]+]]:float64 = Uint32ToFloat64 ([[BITS]])
// CHECK-NEXT: BoxFloat64 ([[UNSIGNED]])

// Remainders of int32 values exit for -0; negating a -0 result negates a double.

function remainder(a, b) {
    return -(a % b);
}
jit.prepare(remainder);
for (let i = 1; i < 10; ++i) remainder(i * 7, i);
jit.compile(remainder);

// CHECK-FUNCTION: remainder
// CHECK-PASS: dead code elimination
// CHECK: [[REMAINDER:v[0-9]+]]:int32 = Int32Mod ({{v[0-9]+}}, {{v[0-9]+}}) [fs0]
// CHECK-NEXT: [[DOUBLE:v[0-9]+]]:float64 = Int32ToFloat64 ([[REMAINDER]])
// CHECK-NEXT: Float64Negate ([[DOUBLE]])

// Updates and comparisons of doubles.

function count(x) {
    let n = x;
    n++;
    return n < 10.5;
}
jit.prepare(count);
for (let i = 0; i < 10; ++i) {
    count(i);
    count(i + 0.25);
}
jit.compile(count);

// CHECK-FUNCTION: count
// CHECK-PASS: dead code elimination
// CHECK: Int32Add ({{v[0-9]+}}, #i32:1)
// CHECK: CallSlowPath PostfixIncrement
// CHECK-NEXT: SlowPathOutput 1
// CHECK: Float64Add ({{v[0-9]+}}, #0x3ff0000000000000)
// CHECK: [[COUNT:v[0-9]+]]:float64 = Phi ({{v[0-9]+}}, {{v[0-9]+}}, {{v[0-9]+}})
// CHECK-NOT: CallSlowPath LessThan
// CHECK: Float64LessThan ([[COUNT]], #0x4025000000000000)
