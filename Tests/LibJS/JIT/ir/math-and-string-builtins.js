// Calls of Math functions of one number and of charCodeAt and charAt are
// built as IR: branches on the callee and the operands go to a cold block that
// calls the instruction's slow path, and the common case computes the result
// in IR.

function floorPlusAbs(x) {
    return Math.floor(x) + Math.abs(x);
}
jit.prepare(floorPlusAbs);
for (let i = 0; i < 10; ++i) floorPlusAbs(i + 0.5);
jit.compile(floorPlusAbs);

// CHECK-FUNCTION: floorPlusAbs
// CHECK-PASS: graph building
// CHECK: BranchBuiltin {{[0-9]+}} ([[FLOOR:v[0-9]+]]) -> [[CALLEE_OK:b[0-9]+]], [[COLD:b[0-9]+]]
// CHECK: [[CALLEE_OK]] <- {{b[0-9]+}}:
// CHECK-NEXT: RefineBuiltin {{[0-9]+}} ([[FLOOR]])
// CHECK-NEXT: BranchInt32Value ([[X:v[0-9]+]]) -> [[INT32:b[0-9]+]], [[OTHER:b[0-9]+]]
// CHECK: [[COLD]] <- {{b[0-9]+}} cold:
// CHECK-NEXT: Jump [[SLOW:b[0-9]+]]
// CHECK: [[INT32]] <- {{b[0-9]+}}:
// CHECK-NEXT: [[INTEGER:v[0-9]+]] = RefineInt32Value ([[X]])
// CHECK-NEXT: UnboxInt32 ([[INTEGER]])
// CHECK: [[OTHER]] <- {{b[0-9]+}}:
// CHECK-NEXT: BranchDouble ([[X]])
// CHECK: [[REFINED:v[0-9]+]] = RefineDouble ([[X]])
// CHECK-NEXT: [[NUMBER:v[0-9]+]]:float64 = UnboxDouble ([[REFINED]])
// CHECK-NEXT: [[FLOORED:v[0-9]+]]:float64 = Float64Floor ([[NUMBER]])
// CHECK-NEXT: BoxFloat64 ([[FLOORED]])
// CHECK: [[SLOW]] <- {{b[0-9]+}}, {{b[0-9]+}} cold:
// CHECK-NEXT: CallSlowPath CallBuiltinMathFloor {{@[0-9]+}} ([[FLOOR]], {{[^)]*}}[[X]])
// CHECK: Float64Abs

function characters(s, i) {
    return s.charCodeAt(i) + s.charAt(i).length;
}
jit.prepare(characters);
for (let i = 0; i < 10; ++i) characters("hello", i % 5);
jit.compile(characters);

// CHECK-FUNCTION: characters
// CHECK-PASS: graph building
// CHECK: BranchBuiltin {{[0-9]+}}
// CHECK: BranchInt32Value ([[I:v[0-9]+]])
// CHECK: [[INTEGER:v[0-9]+]] = RefineInt32Value ([[I]])
// CHECK-NEXT: [[INDEX:v[0-9]+]]:int32 = UnboxInt32 ([[INTEGER]])
// CHECK-NEXT: BranchString ([[S:v[0-9]+]])
// CHECK: [[STRING:v[0-9]+]] = RefineString ([[S]])
// CHECK-NEXT: [[ADDRESS:v[0-9]+]]:pointer = StringAddress ([[STRING]])
// CHECK-NEXT: BranchResolvedString ([[ADDRESS]])
// CHECK: [[READABLE:v[0-9]+]]:pointer = RefineResolvedString ([[ADDRESS]])
// CHECK-NEXT: [[LENGTH:v[0-9]+]]:int32 = StringLength ([[ADDRESS]])
// CHECK-NEXT: BranchIndexInBounds ([[INDEX]], [[LENGTH]])
// CHECK: [[IN_BOUNDS:v[0-9]+]]:int32 = RefineIndexInBounds ([[INDEX]], [[LENGTH]])
// CHECK-NEXT: [[UNIT:v[0-9]+]]:int32 = LoadStringCodeUnit ([[READABLE]], [[IN_BOUNDS]])
// CHECK-NEXT: BoxInt32 ([[UNIT]])
// CHECK: [[CHARACTER:v[0-9]+]]:int32 = LoadStringCodeUnit
// CHECK-NEXT: BranchInt32LessThan ([[CHARACTER]], #i32:128)
// CHECK: [[ASCII:v[0-9]+]]:int32 = RefineInt32LessThan ([[CHARACTER]], #i32:128)
// CHECK-NEXT: SingleCharacterString ([[ASCII]])

console.log(floorPlusAbs(-2.5), characters("abc", 1), characters("abc", 9));

// Int32 arguments take int32 paths: the absolute value of an int32 is one
// (but for the smallest, which goes to the slow path), and its square root
// is that of its double.

function absAndSqrt(n) {
    const i = n | 0;
    return Math.abs(i) + Math.sqrt(i) + Math.ceil(i * 0.5) + Math.round(i * 0.25);
}
jit.prepare(absAndSqrt);
for (let i = 0; i < 10; ++i) absAndSqrt(i - 5);
jit.compile(absAndSqrt);

// CHECK-FUNCTION: absAndSqrt
// CHECK-PASS: graph building
// CHECK: BranchInt32StrictlyEquals ([[I:v[0-9]+]], #i32:-2147483648)
// CHECK: [[NOT_SMALLEST:v[0-9]+]]:int32 = RefineNotInt32StrictlyEquals ([[I]], #i32:-2147483648)
// CHECK-NEXT: Int32Abs ([[NOT_SMALLEST]])
// CHECK: [[DOUBLE:v[0-9]+]]:float64 = Int32ToFloat64 ([[I]])
// CHECK-NEXT: Float64Sqrt ([[DOUBLE]])
// CHECK: Float64Ceil
// CHECK: Float64Round

console.log(absAndSqrt(-9), absAndSqrt(16));
