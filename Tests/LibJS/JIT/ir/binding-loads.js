// A binding of an environment keeps the value it was loaded with, or the
// value stored to it, until something may store to it: a second load of it
// reuses the first.

function makeCounter() {
    let count = 1;
    return function twice() {
        return count + count;
    };
}
const twice = makeCounter();
jit.prepare(twice);
for (let i = 0; i < 10; ++i) twice();
jit.compile(twice);

// CHECK-FUNCTION: twice
// CHECK-PASS: graph building
// CHECK-COUNT-2: LoadEnvironmentBinding
// CHECK-PASS: check elimination
// CHECK: LoadEnvironmentBinding
// CHECK-NOT: LoadEnvironmentBinding
