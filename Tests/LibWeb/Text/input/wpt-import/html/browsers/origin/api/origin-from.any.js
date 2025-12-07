// META: title=`Origin.from()`
// META: script=resources/serializations.js

//
// Invalid Inputs: `null`, `undefined`, invalid URL strings, random objects.
//
const invalidInputs = [
  null,
  undefined,
  1,
  1.1,
  true,
  {},
  Object,
  Origin,
  Origin.from,
];

for (const invalid of invalidInputs) {
  test(t => {
    assert_throws_js(TypeError, _ => Origin.from(invalid));
  }, `Origin.from(${invalid}) throws a TypeError.`);
}

// Specific object types are tested in `origin-from-*.js` in this directory.
