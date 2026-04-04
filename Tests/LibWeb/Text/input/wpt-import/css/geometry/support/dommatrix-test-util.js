// This formats dict as a string suitable as test name.
// format_value() is provided by testharness.js,
// which also preserves sign for -0.
function format_dict(dict) {
  const props = [];
  for (let prop in dict) {
    props.push(`${prop}: ${format_value(dict[prop])}`);
  }
  return `{${props.join(', ')}}`;
}

// Create a normal JS object with the expected properties
// from a dict with only m11..m44 specified (not a..f).
function matrix3D(dict) {
  const matrix = {m11: 1, m12: 0, m13: 0, m14: 0,
                  m21: 0, m22: 1, m23: 0, m24: 0,
                  m31: 0, m32: 0, m33: 1, m34: 0,
                  m41: 0, m42: 0, m43: 0, m44: 1}
  for (let member in dict) {
    matrix[member] = dict[member];
  }
  matrix.is2D = false;
  matrix.a = matrix.m11;
  matrix.b = matrix.m12;
  matrix.c = matrix.m21;
  matrix.d = matrix.m22;
  matrix.e = matrix.m41;
  matrix.f = matrix.m42;
  return matrix;
}

function matrix2D(dict) {
  const matrix = matrix3D(dict);
  matrix.is2D = true;
  return matrix;
}

function checkMatrix(actual, expected, { epsilon = Number.MIN_VALUE } = {}) {
  for (let member in expected) {
    if (epsilon && typeof expected[member] === "number") {
      assert_approx_equals(actual[member], expected[member], epsilon, member);
    } else {
      assert_equals(actual[member], expected[member], member);
    }
  }
}

function identity() {
    return new DOMMatrix(
    [1, 0, 0, 0,
        0, 1, 0 ,0,
        0, 0, 1, 0,
        0, 0, 0, 1]);
}

function update(matrix, f) {
    f(matrix);
    return matrix;
}
