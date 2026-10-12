// Keyed accesses are made of small nodes: a check of the elements kind,
// the element counts, bounds checks against them, the load or store, and
// for holey elements a check for a hole. Sites that speculation does not
// cover branch on the elements kinds and keys their feedback saw, and take
// the slow path, in cold blocks, for anything else.

function sum(a) {
    let s = 0;
    for (let i = 0; i <= a.length; i++) {
        const v = a[i];
        if (v !== undefined) s += v;
    }
    return s;
}
jit.prepare(sum);
for (let i = 0; i < 10; ++i) sum([1, 2, 3]);
jit.compile(sum);

// CHECK-FUNCTION: sum
// CHECK-PASS: frame initialization
// CHECK: CheckArray
// CHECK: LoadElementsLength
// CHECK: CheckNonNegativeInt32
// CHECK-NOT: CallSlowPath GetLength
// CHECK: BranchElementsKind Packed
// CHECK: LoadElementsCapacity
// CHECK-NEXT: Uint32Min
// CHECK-NEXT: BranchIndexInBounds
// CHECK: LoadElementAt Packed
// CHECK: cold:
// CHECK: CallSlowPath

function get(o, k) {
    return o[k];
}
jit.prepare(get);
const object = { a: 1, b: 2, c: 3 };
for (let i = 0; i < 10; ++i) get(object, "abc"[i % 3]);
jit.compile(get);

// CHECK-FUNCTION: get
// CHECK-PASS: frame initialization
// CHECK-NOT: BranchElementsKind
// CHECK: ProbeKeyedCache cache
// CHECK: cold:
// CHECK-NEXT: CallSlowPath GetByValue

function set(o, k, v) {
    o[k] = v;
}
jit.prepare(set);
for (let i = 0; i < 10; ++i) set(object, "abc"[i % 3], i);
jit.compile(set);

// CHECK-FUNCTION: set
// CHECK-PASS: frame initialization
// CHECK: ProbeKeyedStore cache
// CHECK-NEXT: BranchBool

function character(s, i) {
    return s[i];
}
jit.prepare(character);
for (let i = 0; i < 10; ++i) character("hello", i % 5);
jit.compile(character);

// CHECK-FUNCTION: character
// CHECK-PASS: frame initialization
// CHECK: BranchString
// CHECK: StringAddress
// CHECK: BranchResolvedString
// CHECK: StringLength
// CHECK-NEXT: BranchIndexInBounds
// CHECK: LoadStringCodeUnit
// CHECK: SingleCharacterString

// Typed arrays of floats load doubles, which become numbers.

function float(a, i) {
    return a[i];
}
jit.prepare(float);
const floats = new Float64Array([0.5, 1.5, 2.5]);
for (let i = 0; i < 10; ++i) float(floats, i % 3);
jit.compile(float);

// CHECK-FUNCTION: float
// CHECK-PASS: frame initialization
// CHECK: CheckElements TypedArray(Float64)
// CHECK: LoadTypedArrayLength
// CHECK: CheckBounds
// CHECK: LoadElementAt TypedArray(Float64)
// CHECK-NEXT: BoxFloat64

// Stores into typed arrays check that values are numbers and convert them to the kind of the elements.

function storeNumber(a, i, v) {
    a[i] = v;
}
jit.prepare(storeNumber);
const integers = new Int32Array(4);
for (let i = 0; i < 10; ++i) storeNumber(integers, i % 4, i + 0.5);
jit.compile(storeNumber);

// CHECK-FUNCTION: storeNumber
// CHECK-PASS: frame initialization
// CHECK: [[NUMBER:v[0-9]+]]:float64 = CheckNumber
// CHECK-NEXT: [[INTEGER:v[0-9]+]]:int32 = Float64ToInt32 ([[NUMBER]])
// CHECK: CheckElements TypedArray(Int32)
// CHECK: CheckBounds
// CHECK: StoreElementAt TypedArray(Int32)
// CHECK-SAME: [[INTEGER]]

// Stores that exited for a value that is no number take numbers of their kind once recompiled, and leave other values
// to the slow path.

function store(a, i, v) {
    a[i] = v;
}
jit.prepare(store);
for (let i = 0; i < 10; ++i) store(integers, i % 4, i);
jit.compile(store);
store(integers, 0, "x");
jit.discard(store);
jit.compile(store);

// CHECK-FUNCTION: store
// CHECK-PASS: frame initialization
// CHECK: BranchElementsKind TypedArray(Int32)
// CHECK: BranchInt32Value
// CHECK: UnboxInt32
// CHECK: BranchIndexInBounds
// CHECK: StoreElementAt TypedArray(Int32)

// Stores to the index of the next element append like push does, where the
// array has room for it.

function append(a, v) {
    a[a.length] = v;
}
jit.prepare(append);
for (let i = 0; i < 10; ++i) append([1, 2, 3], i);
jit.compile(append);

// CHECK-FUNCTION: append
// CHECK-PASS: frame initialization
// CHECK: CheckArray
// CHECK: [[LENGTH:v[0-9]+]]:int32 = LoadElementsLength
// CHECK: CheckNonNegativeInt32
// CHECK: BranchElementsKind Packed
// CHECK: BranchIndexInBounds ({{v[0-9]+}}, [[LENGTH]])
// CHECK: BranchInt32StrictlyEquals ({{v[0-9]+}}, [[LENGTH]])
// CHECK: CheckAppendableArray
// CHECK: LoadElementsCapacity
// CHECK-NEXT: BranchIndexInBounds
// CHECK: AppendElement

// Holey elements check for holes.

function holey(a, i) {
    return a[i];
}
jit.prepare(holey);
const holes = [1, , 3];
for (let i = 0; i < 10; ++i) holey(holes, i % 2 ? 0 : 2);
jit.compile(holey);

// CHECK-FUNCTION: holey
// CHECK-PASS: frame initialization
// CHECK: CheckElements Holey
// CHECK: LoadElementAt Holey
// CHECK-NEXT: CheckNotHole

// Two accesses of the same array share its kind check and element counts, which move out of the loop with the bound
// of indices, the smaller of the two, and every load stays below the check it relies on.

function pairs(a, n) {
    let s = 0;
    for (let i = 0; i < n; i++) s = (s + a[i] + a[i + 1]) | 0;
    return s;
}
jit.prepare(pairs);
for (let i = 0; i < 10; ++i) pairs([1, 2, 3, 4], 3);
jit.compile(pairs);

// CHECK-FUNCTION: pairs
// CHECK-PASS: graph building
// CHECK-COUNT-2: CheckElements Packed
// CHECK-PASS: loop invariant code motion
// CHECK: [[OBJECT:v[0-9]+]] = CheckElements Packed
// CHECK-NEXT: [[ADDRESS:v[0-9]+]]:pointer = CellAddress ([[OBJECT]])
// CHECK-NEXT: [[LENGTH:v[0-9]+]]:int32 = LoadElementsLength ([[ADDRESS]])
// CHECK-NEXT: [[CAPACITY:v[0-9]+]]:int32 = LoadElementsCapacity ([[ADDRESS]])
// CHECK-NEXT: [[BOUND:v[0-9]+]]:int32 = Uint32Min ([[LENGTH]], [[CAPACITY]])
// CHECK: loop:
// CHECK: [[INDEX:v[0-9]+]]:int32 = CheckBounds ({{v[0-9]+}}, [[BOUND]])
// CHECK-NEXT: LoadElementAt Packed ([[ADDRESS]], [[INDEX]])
// CHECK-NOT: CheckElements
// CHECK-NOT: LoadElementsLength
// CHECK: [[NEXT:v[0-9]+]]:int32 = CheckBounds ({{v[0-9]+}}, [[BOUND]])
// CHECK-NEXT: LoadElementAt Packed ({{v[0-9]+}}, [[NEXT]])
