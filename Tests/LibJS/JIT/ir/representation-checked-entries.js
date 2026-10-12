// OPTIONS: stress-osr
// A loop invariant value that the loop entry and an on-stack replacement entry bring in tagged becomes an int32 phi
// where the code checks it to be an int32, even only on some iterations: its tagged values are checked where they
// enter, once, instead of in the loop.

function addOnOddIterations(n, k) {
    const step = k | 0;
    let sum = 0;
    for (let i = 0; i < n; i = (i + 1) | 0) {
        if (i & 1) sum = (sum + step) | 0;
    }
    return sum;
}
jit.prepare(addOnOddIterations);
for (let i = 0; i < 10; ++i) addOnOddIterations(6, 3);
jit.compile(addOnOddIterations);

// CHECK-FUNCTION: addOnOddIterations
// CHECK-PASS: graph building
// CHECK: loop:
// CHECK: CheckInt32
// CHECK-PASS: representation selection
// CHECK: loop:
// CHECK-NEXT: :int32 = Phi
// CHECK-NEXT: [[STEP:v[0-9]+]]:int32 = Phi
// CHECK: Int32WrappingAdd ({{v[0-9]+}}, [[STEP]])
// CHECK: osr entry at pc
