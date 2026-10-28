// META: title=`Origin.from(HyperlinkElementUtils)`
// META: script=resources/serializations.js

test(t => {
  const invalid = document.createElement("a");
  assert_throws_js(TypeError, _ => Origin.from(invalid));
}, `Origin.from(<a>) throws.`);

test(t => {
  const invalid = document.createElement("area");
  assert_throws_js(TypeError, _ => Origin.from(invalid));
}, `Origin.from(<area>) throws.`);

test(t => {
  const invalid = document.createElementNS('http://www.w3.org/2000/svg', 'a');
  assert_throws_js(TypeError, _ => Origin.from(invalid));
}, `Origin.from(SVG <a>) throws.`);

test(t => {
  const invalid = document.createElementNS('http://www.w3.org/1998/Math/MathML', 'a');
  assert_throws_js(TypeError, _ => Origin.from(invalid));
}, `Origin.from(MathML <a>) throws.`);

for (const opaque of urls.opaque) {
  // <a>
  test(t => {
    const a = document.createElement("a");
    a.href = opaque;
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_true(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_false(origin.isSameOrigin(Origin.from(a)));
  }, `Origin.from(<a href="${opaque}">) returns an opaque origin.`);

  // <area>
  test(t => {
    const area = document.createElement("area");
    area.href = opaque;
    const origin = Origin.from(area);
    assert_true(!!origin);
    assert_true(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_false(origin.isSameOrigin(Origin.from(area)));
  }, `Origin.from(<area href="${opaque}">) returns an opaque origin.`);

  // SVG <a>
  test(t => {
    const a = document.createElementNS('http://www.w3.org/2000/svg', 'a');
    a.href.baseVal = opaque;
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_true(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_false(origin.isSameOrigin(Origin.from(a)));
  }, `Origin.from(SVG <a href="${opaque}">) returns an opaque origin.`);

  test(t => {
    const a = document.createElementNS('http://www.w3.org/2000/svg', 'a');
    a.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', opaque);
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_true(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_false(origin.isSameOrigin(Origin.from(a)));
  }, `Origin.from(SVG <a xlink:href="${opaque}">) returns an opaque origin.`);

  // MathML <a>
  test(t => {
    const a = document.createElementNS('http://www.w3.org/1998/Math/MathML', 'a');
    a.href = opaque;
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_true(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_false(origin.isSameOrigin(Origin.from(a)));
  }, `Origin.from(MathML <a href="${opaque}">) returns an opaque origin.`);
}

for (const tuple of urls.tuple) {
  // <a>
  test(t => {
    const a = document.createElement("a");
    a.href = tuple;
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_false(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_true(origin.isSameOrigin(Origin.from(a)));
  }, `Origin.from(<a href="${tuple}">) returns a tuple origin.`);

  // <area>
  test(t => {
    const area = document.createElement("area");
    area.href = tuple;
    const origin = Origin.from(area);
    assert_true(!!origin);
    assert_false(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_true(origin.isSameOrigin(Origin.from(area)));
  }, `Origin.from(<area href="${tuple}">) returns a tuple origin.`);

  // SVG <a>
  test(t => {
    const a = document.createElementNS('http://www.w3.org/2000/svg', 'a');
    a.href.baseVal = tuple;
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_false(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_true(origin.isSameOrigin(Origin.from(a)));
  }, `Origin.from(SVG <a href="${tuple}">) returns a tuple origin.`);

  test(t => {
    const a = document.createElementNS('http://www.w3.org/2000/svg', 'a');
    a.setAttributeNS('http://www.w3.org/1999/xlink', 'xlink:href', tuple);
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_false(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_true(origin.isSameOrigin(Origin.from(a)));
  }, `Origin.from(SVG <a xlink:href="${tuple}">) returns a tuple origin.`);

  // MathML <a>
  test(t => {
    const a = document.createElementNS('http://www.w3.org/1998/Math/MathML', 'a');
    a.href = tuple;
    const origin = Origin.from(a);
    assert_true(!!origin);
    assert_false(origin.opaque);
    assert_true(origin.isSameOrigin(origin));
    assert_true(origin.isSameOrigin(Origin.from(a)));
  }, `Origin.from(MathML <a href="${tuple}">) returns a tuple origin.`);
}
