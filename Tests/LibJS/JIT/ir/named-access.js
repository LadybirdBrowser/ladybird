// Named property accesses that speculation does not cover probe the
// interpreter's property lookup caches: GetById reads what the cache has
// for objects, and for strings, numbers and booleans, for their
// prototypes, and PutById stores and adds through it. What the probes do
// not handle takes the slow path in a cold block.

function getX(o) {
    return o.x;
}
jit.prepare(getX);
for (let i = 0; i < 20; ++i) getX({ x: i, ["k" + i]: i });
jit.compile(getX);

// CHECK-FUNCTION: getX
// CHECK-PASS: frame initialization
// CHECK: [[PROPERTY:v[0-9]+]] = ProbePropertyCache cache {{[0-9]+}} ([[O:v[0-9]+]])
// CHECK-NEXT: BranchTaggedEquals ([[PROPERTY]], #empty)
// CHECK: cold:
// CHECK-NEXT: CallSlowPath GetById {{@[0-9]+}} ([[O]])

function setX(o, v) {
    o.x = v;
}
jit.prepare(setX);
for (let i = 0; i < 20; ++i) setX({ x: 0, ["k" + i]: i }, i);
jit.compile(setX);

// CHECK-FUNCTION: setX
// CHECK-PASS: frame initialization
// CHECK: ProbePropertyStore cache
// CHECK-NEXT: BranchBool

// The cases of a switch on the shape of an object refine it to have the
// shape, and the loads of each case take the refinement.

function getY(o) {
    return o.y;
}
jit.prepare(getY);
for (let i = 0; i < 10; ++i) getY(i % 2 ? { y: 1 } : { a: 0, y: 2 });
jit.compile(getY);

// CHECK-FUNCTION: getY
// CHECK-PASS: graph building
// CHECK: ShapeSwitch ([[OBJECT:v[0-9]+]], [[ADDRESS:v[0-9]+]])
// CHECK: [[REFINED:v[0-9]+]] = RefineShape {{.*}}([[OBJECT]], [[ADDRESS]])
// CHECK-NEXT: [[CASE_ADDRESS:v[0-9]+]]:pointer = CellAddress ([[REFINED]])
// CHECK-NEXT: LoadNamed {{.*}}([[REFINED]], [[CASE_ADDRESS]])

// Global variables whose caches found nothing to speculate on, like these
// accessors, probe the global variable caches like the interpreter does.

let reads = 0;
Object.defineProperty(globalThis, "tick", {
    get() {
        return ++reads;
    },
    set(value) {
        reads = value;
    },
});

function readAndWrite() {
    tick = tick + 1;
}
jit.prepare(readAndWrite);
for (let i = 0; i < 10; ++i) readAndWrite();
jit.compile(readAndWrite);

// CHECK-FUNCTION: readAndWrite
// CHECK-PASS: frame initialization
// CHECK: [[REALM:v[0-9]+]]:pointer = LoadFrameField Realm
// CHECK-NEXT: [[EXECUTABLE:v[0-9]+]]:pointer = LoadFrameField Executable
// CHECK-NEXT: [[VALUE:v[0-9]+]] = ProbeGlobalCache cache 0 ([[REALM]], [[EXECUTABLE]])
// CHECK-NEXT: BranchTaggedEquals ([[VALUE]], #empty)
// CHECK: ProbeGlobalStore cache 1 ({{v[0-9]+}}, [[REALM]], [[EXECUTABLE]])
// CHECK-NEXT: BranchBool
