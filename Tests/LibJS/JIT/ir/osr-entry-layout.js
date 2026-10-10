// OPTIONS: stress-osr
// On-stack replacement entries run once per entry into the code, so they are laid out last, with the cold code: the
// blocks they lead to start with the register state of a path through the loop instead of the entry's.

function sum(n) {
    let s = 0;
    for (let i = 0; i < n; ++i) s += i;
    return s;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum(5);
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: frame initialization
// CHECK: Return
// CHECK: [[ENTRY:b[0-9]+]] cold:
// CHECK-NEXT: LoadSlot
// CHECK: osr entry at pc {{[0-9]+}}: [[ENTRY]]
