// OPTIONS: stress-osr
// A loop entered by on-stack replacement at its test compares its counter without checking it to be an int32 again,
// so bounds check elimination can drop the increment's overflow check. The bound is a global constant because loops
// with a literal or a parameter as their bound never had that check.

const COUNT = 100;
function sum() {
    let total = 0;
    for (let i = 0; i < COUNT; ++i) total += i;
    return total;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum();
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: frame initialization
// CHECK: loop:
// CHECK: [[NEXT:v[0-9]+]]:int32 = Int32WrappingAdd ({{v[0-9]+}}, #i32:1)
// CHECK-NOT: CheckInt32
// CHECK: BranchInt32LessThan ([[NEXT]], #i32:100)
