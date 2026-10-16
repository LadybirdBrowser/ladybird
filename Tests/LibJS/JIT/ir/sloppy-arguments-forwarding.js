// A sloppy function without parameters has a mapped arguments object that aliases no bindings, so forwarding it with
// apply creates no arguments object.

function target(a, b) {
    return a + b;
}
function forward() {
    return target.apply(this, arguments);
}
jit.prepare(forward);
for (let i = 0; i < 10; ++i) forward(1, i);
jit.compile(forward);

// CHECK-FUNCTION: forward
// CHECK-PASS: graph building
// CHECK-NOT: CreateArguments
// CHECK: LoadArgument
