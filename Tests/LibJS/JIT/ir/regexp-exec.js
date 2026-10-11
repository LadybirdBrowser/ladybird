// Calls of RegExp.prototype.exec of a string are built as a CallIntrinsic node,
// whose helper declines (with the empty value) where RegExpBuiltinExec would run
// code, and a cold block makes the call then.

function countTokens(re, s) {
    let n = 0;
    while (re.exec(s)) ++n;
    return n;
}
jit.prepare(countTokens);
for (let i = 0; i < 10; ++i) countTokens(/a/g, "aaa");
jit.compile(countTokens);

// CHECK-FUNCTION: countTokens
// CHECK-PASS: graph building
// CHECK: [[MATCH:v[0-9]+]] = CallIntrinsic RegExpPrototypeExec ({{v[0-9]+}}, {{v[0-9]+}})
// CHECK-NEXT: BranchTaggedEquals ([[MATCH]], #empty)
// CHECK-NOT: CallNative

console.log(countTokens(/b/g, "abcb"));
