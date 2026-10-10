// Arithmetic of constants is folded.

function constant() {
    const a = 6;
    const b = 7;
    return a * b + 1;
}
jit.prepare(constant);
for (let i = 0; i < 10; ++i) constant();
jit.compile(constant);

// CHECK-FUNCTION: constant
// CHECK-PASS: graph building
// CHECK: Int32Mul (#i32:6, #i32:7)
// CHECK-PASS: frame initialization
// CHECK-NOT: Int32Mul
// CHECK: Return (#43)
