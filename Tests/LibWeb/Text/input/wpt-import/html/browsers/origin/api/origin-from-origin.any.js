// META: title=`Origin.from(URL)`
// META: script=resources/serializations.js

for (const opaque of urls.opaque) {
  test(t => {
    const originFromString = Origin.from(opaque);
    const origin = Origin.from(originFromString);
    assert_true(!!origin);
    assert_true(origin.opaque, "Origin should be opaque.");
    assert_true(origin.isSameOrigin(originFromString));
  }, `Origin.from(Origin.from(${JSON.stringify(opaque)})) is an opaque origin.`);
}

for (const tuple of urls.tuple) {
  test(t => {
    const originFromString = Origin.from(tuple);
    const origin = Origin.from(originFromString);
    assert_true(!!origin);
    assert_false(origin.opaque, "Origin should be opaque.");
    assert_true(origin.isSameOrigin(originFromString));
  }, `Origin.from(Origin.from(${JSON.stringify(tuple)})) is an tuple origin.`);
}
