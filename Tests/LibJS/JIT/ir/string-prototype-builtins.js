// Calls of String.prototype.indexOf, toLowerCase and toUpperCase are built as
// CallIntrinsic nodes for strings, which call the runtime helper of the builtin
// without a native frame. Other receivers and arguments go to a cold block that
// makes the call.

function classIndex(className, name) {
    return (" " + className + " ").indexOf(" " + name + " ");
}
jit.prepare(classIndex);
for (let i = 0; i < 10; ++i) classIndex("a b", "b");
jit.compile(classIndex);

// CHECK-FUNCTION: classIndex
// CHECK-PASS: graph building
// CHECK: CheckValue
// CHECK: BranchString ([[S:v[0-9]+]])
// CHECK: [[STRING:v[0-9]+]] = RefineString ([[S]])
// CHECK: BranchString ([[T:v[0-9]+]])
// CHECK: [[SEARCH:v[0-9]+]] = RefineString ([[T]])
// CHECK-NEXT: [[INDEX:v[0-9]+]]:int32 = CallIntrinsic StringPrototypeIndexOf ([[STRING]], [[SEARCH]], #i32:0)
// CHECK-NEXT: BoxInt32 ([[INDEX]])
// CHECK-NOT: CallNative

function cases(s) {
    return s.toLowerCase() + s.toUpperCase();
}
jit.prepare(cases);
for (let i = 0; i < 10; ++i) cases("aB");
jit.compile(cases);

// CHECK-FUNCTION: cases
// CHECK-PASS: graph building
// CHECK: [[LOWER_STRING:v[0-9]+]] = RefineString
// CHECK-NEXT: CallIntrinsic StringPrototypeToLowerCase ([[LOWER_STRING]])
// CHECK: [[UPPER_STRING:v[0-9]+]] = RefineString
// CHECK-NEXT: CallIntrinsic StringPrototypeToUpperCase ([[UPPER_STRING]])
// CHECK-NOT: CallNative

// Helpers that answer yes or no return a bool, which the code boxes. Absent
// positions and ends are constants the builtin treats the same.

function startsAndTail(s, prefix) {
    return s.startsWith(prefix) ? s.slice(prefix.length) : s.trim();
}
jit.prepare(startsAndTail);
for (let i = 0; i < 10; ++i) startsAndTail(i % 2 ? "ab" : " c ", "a");
jit.compile(startsAndTail);

// CHECK-FUNCTION: startsAndTail
// CHECK-PASS: graph building
// CHECK: [[STARTS:v[0-9]+]]:bool = CallIntrinsic StringPrototypeStartsWith ({{v[0-9]+}}, {{v[0-9]+}}, #i32:0)
// CHECK-NEXT: BoxBool ([[STARTS]])
// CHECK: CallIntrinsic StringPrototypeSlice ({{v[0-9]+}}, {{v[0-9]+}}, #i32:2147483647)
// CHECK: CallIntrinsic StringPrototypeTrim ({{v[0-9]+}})
// CHECK-NOT: CallNative

console.log(classIndex("x y z", "y"), cases("MiXeD"), startsAndTail("abc", "a"), startsAndTail(" x ", "a"));
