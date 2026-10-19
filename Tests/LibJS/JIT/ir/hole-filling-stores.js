// Stores into holes of holey arrays fill them inline where the array can get
// a new element without running any code. Stores that need more storage, and
// stores to the last index, which may make the elements packed, call the
// runtime in cold blocks, which runs no code for such arrays either. Holes of
// other objects exit, so no path through the store runs other code.

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
// CHECK-NEXT: BranchIndexInBounds ([[INDEX:v[0-9]+]], [[LENGTH]])
// CHECK-NOT: cold
// CHECK: [[CAPACITY:v[0-9]+]]:int32 = LoadElementsCapacity ([[ADDRESS]])
// CHECK-NEXT: BranchIndexInBounds ([[INDEX]], [[CAPACITY]])
// CHECK-NOT: cold
// CHECK: [[OLD:v[0-9]+]] = LoadElementAt Holey ([[ADDRESS]], [[INDEX]])
// CHECK-NEXT: BranchTaggedEquals ([[OLD]], #empty)
// CHECK-NOT: cold
// CHECK: BranchHoleFillableArray ([[ADDRESS]])
// CHECK-NOT: cold
// CHECK: [[LAST:v[0-9]+]]:int32 = Int32WrappingSub ([[LENGTH]], #i32:1)
// CHECK-NEXT: BranchIndexInBounds ([[INDEX]], [[LAST]])
// CHECK-NOT: cold
// CHECK: StoreElementAt Holey ([[ADDRESS]], [[INDEX]],
// CHECK: cold:
// CHECK-NOT: StoreElementAt
// CHECK: CallStoreHoleyElement ([[ADDRESS]], [[INDEX]],
// CHECK-COUNT-2: CallStoreHoleyElement
// CHECK-COUNT-0: CallSlowPath PutByValue
