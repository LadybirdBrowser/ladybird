// Calls of Math.imul, also through another name like in asm.js code, multiply int32 arguments inline, keeping the low
// 32 bits of the product.

const imul = Math.imul;
function multiply(a, b) {
    return imul(a, b);
}
jit.prepare(multiply);
for (let i = 0; i < 10; ++i) multiply(i, 3);
jit.compile(multiply);

// CHECK-FUNCTION: multiply
// CHECK-PASS: graph building
// CHECK: [[A:v[0-9]+]]:int32 = CheckInt32
// CHECK: [[B:v[0-9]+]]:int32 = CheckInt32
// CHECK-NEXT: Int32WrappingMul ([[A]], [[B]])
// CHECK-NOT: Call
// CHECK: Return
