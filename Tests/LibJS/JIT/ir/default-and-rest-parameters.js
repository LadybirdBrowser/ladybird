// A default parameter value is stored to its argument slot for the rest parameter's slow path, which reads the
// frame. The frame is initialized right before that slow path, after the store: it leaves the arguments as they are.
// The script is strict: functions with a legacy `arguments` property store the value where they assign it instead.

"use strict";

function withDefault(a, b = a + 1, ...rest) {
    return a + b + rest.length;
}
jit.prepare(withDefault);
for (let k = 0; k < 10; ++k) withDefault(k);
jit.compile(withDefault);

// CHECK-FUNCTION: withDefault
// CHECK-PASS: frame initialization
// CHECK: StoreSlot a1
// CHECK-NEXT: InitializeFrame
// CHECK-NEXT: Generic CreateRestParams
