// OPTIONS: stress-osr
// An on-stack replacement entry at the test at the bottom of a loop that reads the bound from a property runs the
// test's branch itself and enters the loop's body, while the code before the loop enters it at the test. Loop rotation
// copies the test for the way in from before the loop, so that the body becomes the loop's only entry, and loop
// invariant code motion moves the loads of `this` out of it.

function Holder(values) {
    this.values = values;
    this.count = values.length;
}
Holder.prototype.sum = function sum() {
    let total = 0;
    for (let i = 0; i < this.count; ++i) total += this.values[i];
    return total;
};
const holder = new Holder([1, 2, 3, 4]);
jit.prepare(Holder.prototype.sum);
for (let i = 0; i < 10; ++i) holder.sum();
jit.compile(Holder.prototype.sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: loop rotation
// CHECK: osr entry at pc
// CHECK-PASS: loop invariant code motion
// CHECK: CheckShape
// CHECK: CheckShape
// CHECK: CheckShape
// CHECK: loop:
// CHECK-NOT: CheckShape
// CHECK-NOT: LoadNamed
// CHECK: LoadElementAt Packed
// CHECK-NOT: CheckShape
// CHECK-NOT: LoadNamed
// CHECK: osr entry at pc
