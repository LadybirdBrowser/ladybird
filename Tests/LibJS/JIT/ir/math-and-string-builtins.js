// Calls of Math functions of one number and of charCodeAt and charAt are
// built as IR: the common case computes the result in IR, and branches on the
// callee and the operands go to a cold block. Math functions exit there,
// unless that happened at the site before, and sites that only saw the builtin
// check the callee against it.

function floorPlusAbs(x) {
    return Math.floor(x) + Math.abs(x);
}
jit.prepare(floorPlusAbs);
for (let i = 0; i < 10; ++i) floorPlusAbs(i + 0.5);
jit.compile(floorPlusAbs);

// CHECK-FUNCTION: floorPlusAbs
// CHECK-PASS: graph building
// CHECK: CheckValue {{0x[0-9a-f]+}} ({{v[0-9]+}})
// CHECK-NEXT: BranchInt32Value ([[X:v[0-9]+]]) -> [[INT32:b[0-9]+]], [[OTHER:b[0-9]+]]
// CHECK: [[INT32]] <- {{b[0-9]+}}:
// CHECK-NEXT: [[INTEGER:v[0-9]+]] = RefineInt32Value ([[X]])
// CHECK-NEXT: UnboxInt32 ([[INTEGER]])
// CHECK: [[OTHER]] <- {{b[0-9]+}}:
// CHECK-NEXT: BranchDouble ([[X]]) -> [[DOUBLE:b[0-9]+]], [[COLD:b[0-9]+]]
// CHECK: [[DOUBLE]] <- {{b[0-9]+}}:
// CHECK-NEXT: [[REFINED:v[0-9]+]] = RefineDouble ([[X]])
// CHECK-NEXT: [[NUMBER:v[0-9]+]]:float64 = UnboxDouble ([[REFINED]])
// CHECK-NEXT: [[FLOORED:v[0-9]+]]:float64 = Float64Floor ([[NUMBER]])
// CHECK-NEXT: BoxFloat64 ([[FLOORED]])
// CHECK: [[COLD]] <- {{b[0-9]+}} cold:
// CHECK-NEXT: Exit SlowPath
// CHECK: Float64Abs
// CHECK-NOT: CallSlowPath

// A site that exited calls the slow path instead.

function floorOf(x) {
    return Math.floor(x);
}
jit.prepare(floorOf);
for (let i = 0; i < 10; ++i) floorOf(i + 0.5);
jit.compile(floorOf);
floorOf("1.5");
jit.discard(floorOf);
jit.compile(floorOf);

// CHECK-FUNCTION: floorOf
// CHECK-PASS: graph building
// CHECK: BranchDouble ({{v[0-9]+}}) -> {{b[0-9]+}}, [[COLD:b[0-9]+]]
// CHECK: [[COLD]] <- {{b[0-9]+}} cold:
// CHECK-NEXT: CallSlowPath Call @
// CHECK-NOT: Exit SlowPath

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

console.log(floorPlusAbs(-2.5), floorOf(-2.5), characters("abc", 1), characters("abc", 9));

// Int32 arguments take int32 paths: the absolute value of an int32 is the
// larger of it and its negation (which overflows, and exits, for the
// smallest), and its square root is that of its double.

function absAndSqrt(n) {
    const i = n | 0;
    return Math.abs(i) + Math.sqrt(i) + Math.ceil(i * 0.5) + Math.round(i * 0.25);
}
jit.prepare(absAndSqrt);
for (let i = 0; i < 10; ++i) absAndSqrt(i - 5);
jit.compile(absAndSqrt);

// CHECK-FUNCTION: absAndSqrt
// CHECK-PASS: graph building
// CHECK: [[NEGATED:v[0-9]+]]:int32 = Int32Sub (#i32:0, [[I:v[0-9]+]])
// CHECK-NEXT: Int32Max ([[I]], [[NEGATED]])
// CHECK: [[DOUBLE:v[0-9]+]]:float64 = Int32ToFloat64 ([[I]])
// CHECK-NEXT: Float64Sqrt ([[DOUBLE]])
// CHECK: Float64Ceil
// CHECK: Float64Round

console.log(absAndSqrt(-9), absAndSqrt(16));

// Math.exp, log, sin, cos, tan and pow of numbers call the runtime's functions
// of doubles, which only compute: the code after them keeps what it knew, and
// a call of loop invariant numbers moves out of the loop.

function bias(base, values) {
    let sum = 0;
    for (let i = 0; i < values.length; ++i) sum += Math.pow(values[i], Math.log(1 - base * 0.5) / Math.LN2);
    return sum;
}
jit.prepare(bias);
for (let i = 0; i < 10; ++i) bias(0.25, [0.5, 0.75]);
jit.compile(bias);

// CHECK-FUNCTION: bias
// CHECK-PASS: graph building
// CHECK: CallFloat64Log
// CHECK: CallFloat64Pow
// CHECK-NOT: CallSlowPath Call @
// CHECK-PASS: loop invariant code motion
// CHECK: CallFloat64Log
// CHECK: loop:
// CHECK-NOT: CallFloat64Log
// CHECK: CallFloat64Pow

function trigonometry(x) {
    return Math.sin(x) + Math.cos(x) + Math.tan(x) + Math.exp(x);
}
jit.prepare(trigonometry);
for (let i = 0; i < 10; ++i) trigonometry(i * 0.5);
jit.compile(trigonometry);

// CHECK-FUNCTION: trigonometry
// CHECK-PASS: dead code elimination
// CHECK: CallFloat64Sin
// CHECK: CallFloat64Cos
// CHECK: CallFloat64Tan
// CHECK: CallFloat64Exp
// CHECK-NOT: CallSlowPath

console.log(bias(0.25, [0.5, 0.75]), trigonometry(0.5), trigonometry(2));

// Calls that name the builtins otherwise are built from the callee that their
// feedback saw.

const math = Math;
const fromCharCode = String.fromCharCode;
function otherNames(x, s) {
    return math.floor(x) + math.pow(x, 2) + s["charCodeAt"](1) + fromCharCode(65);
}
jit.prepare(otherNames);
for (let i = 0; i < 10; ++i) otherNames(i + 0.5, "hello");
jit.compile(otherNames);

// CHECK-FUNCTION: otherNames
// CHECK-PASS: graph building
// CHECK: Float64Floor
// CHECK: CallFloat64Pow
// CHECK: LoadStringCodeUnit
// CHECK: SingleCharacterString

console.log(otherNames(2.5, "abc"));
