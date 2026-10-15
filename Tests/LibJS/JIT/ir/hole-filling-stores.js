// Stores into holes of holey arrays fill them inline where the array can get
// a new element without running any code. Holes of other objects, stores that
// need more storage, and stores to the last index, which may make the
// elements packed, take the slow path, in cold blocks.

function fill(n) {
    const a = new Array(n);
    for (let i = 0; i < n; ++i) a[i] = i;
    return a;
}
jit.prepare(fill);
for (let i = 0; i < 10; ++i) fill(20);
jit.compile(fill);

// CHECK-FUNCTION: fill
// CHECK-PASS: frame initialization
// CHECK: BranchElementsKind Holey
// CHECK: [[ADDRESS:v[0-9]+]]:pointer = CellAddress
// CHECK-NEXT: [[LENGTH:v[0-9]+]]:int32 = LoadElementsLength ([[ADDRESS]])
// CHECK: [[OLD:v[0-9]+]] = LoadElementAt Holey ([[ADDRESS]], [[INDEX:v[0-9]+]])
// CHECK-NEXT: BranchTaggedEquals ([[OLD]], #empty)
// CHECK-NOT: cold
// CHECK: BranchHoleFillableArray ([[ADDRESS]])
// CHECK-NOT: cold
// CHECK: [[LAST:v[0-9]+]]:int32 = Int32WrappingSub ([[LENGTH]], #i32:1)
// CHECK-NEXT: BranchIndexInBounds ([[INDEX]], [[LAST]])
// CHECK-NOT: cold
// CHECK: StoreElementAt Holey ([[ADDRESS]], [[INDEX]],
// CHECK-NOT: CallSlowPath
// CHECK: cold:
// CHECK-NOT: StoreElementAt
// CHECK: CallSlowPath PutByValue
// CHECK-COUNT-1: CallSlowPath PutByValue
