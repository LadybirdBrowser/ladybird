// Map.prototype.has and Map.prototype.get, where the call sites saw only them, become calls of their runtime helpers,
// which decline (with the empty value) for receivers that are no Map, so that the slow path calls the builtin.

function lookup(map, key) {
    return map.has(key) ? map.get(key) : 0;
}
jit.prepare(lookup);
const map = new Map([
    [1, 10],
    [2, 20],
]);
for (let i = 0; i < 10; ++i) lookup(map, i & 3);
jit.compile(lookup);

// CHECK-FUNCTION: lookup
// CHECK-PASS: graph building
// CHECK: [[HAS:v[0-9]+]] = CallIntrinsic MapPrototypeHas ([[MAP:v[0-9]+]], [[KEY:v[0-9]+]])
// CHECK-NEXT: BranchTaggedEquals ([[HAS]], #empty)
// CHECK: [[GET:v[0-9]+]] = CallIntrinsic MapPrototypeGet ([[MAP]], [[KEY]])
// CHECK-NEXT: BranchTaggedEquals ([[GET]], #empty)

console.log(lookup(map, 2));
