// A generator suspends in compiled code like the interpreter's Yield: it writes the slots live at the continuation to
// its frame, records the continuation, and returns the value. It resumes at an entry block of the continuation, which
// loads those slots from the frame like an on-stack replacement entry, and reads the completion it resumed with from
// its generator object.

function* counter(limit) {
    for (let i = 0; i < limit; ++i) yield i;
}
jit.prepare(counter);
for (let i = 0; i < 10; ++i) [...counter(5)];
jit.compile(counter);

// CHECK-FUNCTION: counter
// CHECK-PASS: graph building
// CHECK: LoadSlot l0
// CHECK-NEXT: LoadSlot a0
// CHECK-NEXT: Jump [[CONTINUATION:b[0-9]+]]
// CHECK: InitializeFrame
// CHECK-NEXT: LoadSlot r2
// CHECK-NEXT: Suspend pc [[START:[0-9]+]]
// CHECK-NEXT: Return (#undefined)
// CHECK: Suspend completed
// CHECK-NEXT: Return (#undefined)
// CHECK: StoreSlot l0 ([[VALUE:v[0-9]+]])
// CHECK-NEXT: Suspend pc [[NEXT:[0-9]+]]
// CHECK-NEXT: Return ([[VALUE]])
// CHECK: [[CONTINUATION]] (pc [[NEXT]]) <- [[RESUME:b[0-9]+]]:
// CHECK: [[GENERATOR:v[0-9]+]]:pointer = RefineGeneratorObject
// CHECK-NEXT: LoadGeneratorCompletionType ([[GENERATOR]])
// CHECK-NEXT: BoxInt32
// CHECK-NEXT: LoadGeneratorCompletionValue ([[GENERATOR]])
// CHECK: osr entry at pc [[START]]: {{b[0-9]+}}
// CHECK-NEXT: osr entry at pc [[NEXT]]: [[RESUME]]

console.log([...counter(3)].join());
