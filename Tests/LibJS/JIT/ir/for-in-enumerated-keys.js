// A for-in loop's keyed loads and hasOwnProperty() calls of its key get the
// loop's property iterator cache and the key's index as inputs, so code
// generation can find the property at the offset the cache has for the key.

function count(o) {
    let n = 0;
    for (const k in o) if (o.hasOwnProperty(k) && o[k] != null) n++;
    return n;
}
jit.prepare(count);
for (let i = 0; i < 10; ++i) count({ a: 1, b: null, c: i });
jit.compile(count);

// CHECK-FUNCTION: count
// CHECK-PASS: enumerated keys
// CHECK: [[CACHE:v[0-9]+]] = CallSlowPath GetObjectPropertyIterator
// CHECK: [[KEY:v[0-9]+]] = LoadPropertyIteratorKey ([[CACHE]], [[INDEX:v[0-9]+]])
// CHECK: ProbeHasOwnProperty ({{v[0-9]+}}, [[KEY]], [[CACHE]], [[INDEX]])
// CHECK: ProbeKeyedCache cache {{[0-9]+}} ({{v[0-9]+}}, [[KEY]], [[CACHE]], [[INDEX]])
