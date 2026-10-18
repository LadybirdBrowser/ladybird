// An exception thrown in compiled code leaves it, and the interpreter runs the Catch of the handler, which continues
// the frame in compiled code at an entry block after it. The entry loads the slots live there from the frame, like the
// resume entry of a generator, and jumps to the block that continues the handler. A handler that never caught an
// exception gets no entry, and its blocks are not built.

function thrower(x) {
    if (x & 1) throw x;
    return x;
}

function sum(n) {
    let total = 0;
    for (let i = 0; i < n; ++i) {
        try {
            total += thrower(i);
        } catch (e) {
            total -= e;
        }
        try {
            total += thrower(0);
        } catch (e) {
            total -= e;
        }
    }
    return total;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum(10);
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: graph building
// CHECK: {{^}}b0:
// CHECK: {{^}}[[ENTRY:b[0-9]+]]:{{$}}
// CHECK-NEXT: LoadSlot
// CHECK: Jump [[HANDLER:b[0-9]+]]
// CHECK: {{^}}[[HANDLER]] (pc [[PC:[0-9]+]]) <- [[ENTRY]]:
// CHECK-NEXT: CheckInt32
// CHECK: Int32Sub
// CHECK: osr entry at pc [[PC]]: [[ENTRY]]
// CHECK-COUNT-1: osr entry at pc
// CHECK-COUNT-1: Int32Sub

console.log(sum(100));
