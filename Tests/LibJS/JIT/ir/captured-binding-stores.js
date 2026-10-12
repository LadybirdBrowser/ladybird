// Stores to bindings of the closure's environments that their scope declares mutable, like the `var` stack pointers of
// asm.js modules, skip the check of the binding's mutability. They still check it is initialized.

function outer() {
    var top = 0;
    function store(value) {
        top = value;
    }
    return store;
}
const store = outer();
jit.prepare(store);
for (let i = 0; i < 10; ++i) store(i);
jit.compile(store);

// CHECK-FUNCTION: store
// CHECK-PASS: graph building
// CHECK-NOT: BindingMutable
// CHECK: BranchTaggedNotEquals
// CHECK: StoreEnvironmentBinding
