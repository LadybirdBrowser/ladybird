// META: title=`Origin.from(HTMLHyperlinkElementUtils)`
// META: script=resources/serializations.js

test(t => {
  const invalid = document.createElement("a");
  assert_throws_js(TypeError, _ => Origin.from(invalid));
}, `Origin.from(<a>) throws.`);

test(t => {
  const invalid = document.createElement("area");
  assert_throws_js(TypeError, _ => Origin.from(invalid));
}, `Origin.from(<area>) throws.`);

for (const opaque of urls.opaque) {
  // <a>
  test(t => {
    const a = document.createElement("a");
    a.href = opaque;
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_true(origin.opaque);
  }, `Origin.from(<a href="${opaque}">) returns an opaque origin.`);

  // <area>
  test(t => {
    const area = document.createElement("area");
    area.href = opaque;
    const origin = Origin.from(area);
    assert_true(!!origin);
    assert_true(origin.opaque);
  }, `Origin.from(<area href="${opaque}">) returns an opaque origin.`);
}

for (const tuple of urls.tuple) {
  // <a>
  test(t => {
    const a = document.createElement("a");
    a.href = tuple;
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_false(origin.opaque);
  }, `Origin.from(<a href="${tuple}">) returns a tuple origin.`);

  // <area>
  test(t => {
    const area = document.createElement("area");
    area.href = tuple;
    const origin = Origin.from(area);
    assert_true(!!origin);
    assert_false(origin.opaque);
  }, `Origin.from(<area href="${tuple}">) returns a tuple origin.`);
}


