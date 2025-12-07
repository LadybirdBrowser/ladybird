// META: title=`Origin.from(URL)`
// META: script=resources/serializations.js

for (const invalid of urls.invalid) {
  test(t => {
    assert_throws_js(TypeError, _ => Origin.from(new URL(invalid)));
  }, `Origin.from(${JSON.stringify(invalid)}) throws a TypeError.`);
}

for (const opaque of urls.opaque) {
  test(t => {
    const origin = Origin.from(new URL(opaque));
    assert_true(!!origin);
    assert_true(origin.opaque, "Origin should be opaque.");
  }, `Origin.from(${JSON.stringify(opaque)}) is an opaque origin.`);
}

for (const tuple of urls.tuple) {
  test(t => {
    const origin = Origin.from(new URL(tuple));
    assert_true(!!origin);
    assert_false(origin.opaque, "Origin should not be opaque.");
  }, `Origin.from(${JSON.stringify(tuple)}) is an opaque origin.`);
}
