// A call whose callee the code knows exactly inlines it, even where the call site saw other callees.

function apply(f, x) {
    return f(x);
}
function increment(x) {
    return x + 1;
}
function decrement(x) {
    return x - 1;
}
const doubled = x => x * 2;
function caller(n) {
    let sum = 0;
    for (let i = 0; i < n; i++) sum += apply(doubled, i);
    return sum;
}
jit.prepare(caller);
jit.prepare(apply);
for (let i = 0; i < 10; ++i) {
    apply(increment, i);
    apply(decrement, i);
    caller(10);
}
jit.compile(caller);

// CHECK-FUNCTION: caller
// CHECK-PASS: graph building
// CHECK-NOT: CallDirect
// CHECK-NOT: CallSlowPath Call
// CHECK: Int32Mul

// A known builtin that takes a callback still inlines the calls of the callback its call site passed it.

const todos = {
    items: [{ completed: false }, { completed: true }],
    active: function () {
        return this.items.filter(function (item) {
            return !item.completed;
        });
    },
    count: function () {
        return this.active().length;
    },
};
jit.prepare(todos.count);
for (let i = 0; i < 10; ++i) todos.count();
jit.compile(todos.count);

// CHECK-FUNCTION: count
// CHECK-PASS: graph building
// CHECK-NOT: CallDirect @e2
// CHECK: CallArrayPush
