// An async function runs from its start into its body, which has no initial suspension. It suspends at an await in
// compiled code like the interpreter's Await: it writes the slots live at the continuation to its frame, records the
// continuation and that the value is awaited, and returns the value. It resumes at an entry block of the continuation,
// which loads those slots from the frame like a generator's resume entry, and reads the completion it resumed with
// from its generator object.

async function sum(values) {
    let total = 0;
    for (const value of values) total += await value;
    return total;
}
jit.prepare(sum);
const warm_up = [];
for (let i = 0; i < 10; ++i) warm_up.push(sum([1, 2, 3]));

// CHECK-FUNCTION: sum
// CHECK-PASS: graph building
// CHECK: LoadSlot r5
// CHECK-NEXT: LoadSlot r6
// CHECK-NEXT: LoadSlot r7
// CHECK-NEXT: LoadSlot r12
// CHECK-NEXT: Jump [[CONTINUATION:b[0-9]+]]
// CHECK: InitializeFrame
// CHECK-NOT: Suspend
// CHECK: [[VALUE:v[0-9]+]] = EmptyToUndefined
// CHECK-NEXT: Suspend pc [[NEXT:[0-9]+]] await
// CHECK-NEXT: Return ([[VALUE]])
// CHECK: [[CONTINUATION]] (pc [[NEXT]]) <- [[RESUME:b[0-9]+]]:
// CHECK: [[GENERATOR:v[0-9]+]]:pointer = RefineGeneratorObject
// CHECK-NEXT: LoadGeneratorCompletionType ([[GENERATOR]])
// CHECK-NEXT: BoxInt32
// CHECK-NEXT: LoadGeneratorCompletionValue ([[GENERATOR]])
// CHECK-NOT: Exit NoFeedback
// CHECK: Suspend completed
// CHECK-NEXT: Return
// CHECK-NOT: osr entry
// CHECK: osr entry at pc [[NEXT]]: [[RESUME]]
// CHECK-NOT: osr entry

Promise.all(warm_up).then(() => {
    jit.compile(sum);
    sum([4, 5, 6]).then(total => console.log(total));
});
