// Bitwise operations on booleans, like `(a < b) & (c < d)` and `(a < b) | 0` in asm.js code, take the booleans as the
// int32 values 0 and 1 instead of calling the generic operation.

function bothLess(a, b, c, d) {
    a = +a;
    b = +b;
    c = +c;
    d = +d;
    return ((a < b) & (c < d)) ^ true ^ ((a > d) | 0) ^ ~(b > c);
}
jit.prepare(bothLess);
for (let i = 0; i < 10; ++i) bothLess(i, 3, 0.5, i);
jit.compile(bothLess);

// CHECK-FUNCTION: bothLess
// CHECK-PASS: graph building
// CHECK: [[X:v[0-9]+]]:int32 = BoolToInt32
// CHECK: [[Y:v[0-9]+]]:int32 = BoolToInt32
// CHECK: Int32BitwiseAnd
// CHECK: Int32BitwiseXor ({{v[0-9]+}}, #i32:1)
// CHECK-NOT: CallSlowPath
// CHECK: Return
