// Calls of runtime helpers that run no code, like ToBoolean for the truthiness
// of a value that may be an object, write only what they say they do, so loads
// they do not write move out of loops around them.

function weighNodes(node, object) {
    let total = 0;
    for (; node; node = node.next) total = (total + object.weight) | 0;
    return total;
}
jit.prepare(weighNodes);
const list = { next: { next: { next: null } } };
for (let i = 0; i < 10; ++i) weighNodes(list, { weight: 2 });
jit.compile(weighNodes);

// CHECK-FUNCTION: weighNodes
// CHECK-PASS: global value numbering
// CHECK: loop:
// CHECK: ToBoolean
// CHECK: LoadNamed +0
// CHECK-PASS: loop invariant code motion
// CHECK: [[WEIGHT:v[0-9]+]] = LoadNamed +0
// CHECK-NEXT: [[INTEGER:v[0-9]+]]:int32 = CheckInt32 ([[WEIGHT]])
// CHECK: loop:
// CHECK: ToBoolean
// CHECK: Int32WrappingAdd ({{v[0-9]+}}, [[INTEGER]])

console.log(weighNodes(list, { weight: 3 }));
