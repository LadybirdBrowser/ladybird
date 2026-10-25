// `hasOwnProperty` calls and `in` probe whether the object has the property
// where the VM can tell without running code. Otherwise, `in` takes its slow
// path in a cold block, and the call, whose slow path is a call, exits.

function own(o, k) {
    return o.hasOwnProperty(k);
}
jit.prepare(own);
for (let i = 0; i < 10; ++i) own({ a: 1 }, i % 2 ? "a" : "b");
jit.compile(own);

// CHECK-FUNCTION: own
// CHECK-PASS: frame initialization
// CHECK: CheckShape
// CHECK: [[HAS:v[0-9]+]] = ProbeHasOwnProperty ([[O:v[0-9]+]], [[K:v[0-9]+]])
// CHECK-NEXT: BranchTaggedEquals ([[HAS]], #empty)
// CHECK: cold:
// CHECK-NEXT: Exit SlowPath

function has(o, k) {
    return k in o;
}
jit.prepare(has);
for (let i = 0; i < 10; ++i) has([1, 2], i % 3);
jit.compile(has);

// CHECK-FUNCTION: has
// CHECK-PASS: frame initialization
// CHECK: [[HAS:v[0-9]+]] = ProbeHasProperty ([[O:v[0-9]+]], [[K:v[0-9]+]])
// CHECK-NEXT: BranchTaggedEquals ([[HAS]], #empty)
// CHECK: cold:
// CHECK-NEXT: CallSlowPath In {{@[0-9]+}} ([[K]], [[O]])
