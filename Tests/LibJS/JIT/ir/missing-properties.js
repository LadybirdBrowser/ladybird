// A get of a property that objects of the shapes the cache saw do not have
// is undefined after the shape check, a check that the object has no other
// properties than those of its shape, and the check of its prototype chain,
// without a probe of the cache.

function onClick(o) {
    return o.onClick;
}
jit.prepare(onClick);
for (let i = 0; i < 20; ++i) onClick(i % 2 ? { x: i } : { y: i, x: i });
jit.compile(onClick);

// CHECK-FUNCTION: onClick
// CHECK-PASS: graph building
// CHECK: CheckShape
// CHECK: CheckPropertyAbsenceCacheable
// CHECK-NOT: ProbePropertyCache
// CHECK: Return
