// Direct calls leave the callee's frame unpublished. Its code publishes the
// frame before anything else may see it: allocations only need it published,
// calls need it initialized as well.

function makePoint(x, y) {
    return { x, y };
}
jit.prepare(makePoint);
for (let i = 0; i < 10; ++i) makePoint(i, i);
jit.compile(makePoint);

// CHECK-FUNCTION: makePoint
// CHECK-PASS: frame initialization
// CHECK: PublishFrame
// CHECK-NOT: InitializeFrame
// CHECK: AllocateObject

function callOut(f) {
    return f() + 1;
}
jit.prepare(callOut);
for (let i = 0; i < 10; ++i) callOut(() => i);
jit.compile(callOut);

// CHECK-FUNCTION: callOut
// CHECK-PASS: frame initialization
// CHECK-NOT: PublishFrame
// CHECK: InitializeFrame
