// Additions whose results only reach ToInt32 (the `| 0` of asm.js code, shifts, integer typed array stores) wrap
// instead of exiting on overflow, while sums something reads in full keep checking: the returned sum, and the stored
// one, which the checks of the typed array before the store exit with.

const heap = new Int32Array(16);
function asmStyle(a, b) {
    a = a | 0;
    b = b | 0;
    const index = (a + b) | 0;
    heap[(index + 4) >> 2] = a + 1;
    return a + b;
}
jit.prepare(asmStyle);
for (let i = 0; i < 10; ++i) asmStyle(i, 2);
jit.compile(asmStyle);

// CHECK-FUNCTION: asmStyle
// CHECK-PASS: graph building
// CHECK-COUNT-4: = Int32Add
// CHECK-PASS: truncation
// CHECK-COUNT-2: = Int32WrappingAdd
// CHECK-COUNT-2: = Int32Add
// CHECK-PASS: simplification
// CHECK-NOT: Int32BitwiseOr
// CHECK: frame states:
