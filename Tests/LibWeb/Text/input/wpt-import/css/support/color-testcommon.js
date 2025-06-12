'use strict';

/**
 * Set up a test for color properties that does not expect exact equality for
 * numeric values within the color. This is necessary for color-mix and
 * relative color syntax, which perform float arithmetic on color channels.
 *
 * @param {number} epsilon       Epsilon for comparison of numeric values.
 */
function set_up_fuzzy_color_test(epsilon) {
  if (!epsilon) {
    epsilon = 0.01;
  }

  // The function
  function fuzzy_compare_colors(input, expected) {
    const colorElementDividers = /( |\(|,)/;
    // Return the string stripped of numbers.
    function getNonNumbers(color) {
      return color.replace(/[0-9\.]/g, '');
    }
    // Return an array of all numbers in the color.
    function getNumbers(color) {
      const result = [];
      color.split(colorElementDividers).forEach(element => {
        const numberElement = parseFloat(element);
        if (!isNaN(numberElement)) {
          result.push(numberElement);
        }
      });
      return result;
    }

    try {
      assert_array_approx_equals(getNumbers(input), getNumbers(expected), epsilon, "Numeric parameters are approximately equal.");
      // Assert that the text of the two colors are equal. i.e. colorSpace, colorFunction and format.
      assert_equals(getNonNumbers(input), getNonNumbers(expected), "Color format is correct.");
    } catch (error) {
      throw `Colors do not match.\nActual:   ${input}\nExpected: ${expected}.\n${error}`
    }
  }

  return fuzzy_compare_colors;
}

/**
 * Test the computed value of a color with some tolerance for numeric parameters.
 *
 * @param {string} specified  A specified value for the color.
 * @param {string} computed   The expected computed color. If omitted, defaults
 *                            to the default test_computed_value test, as
 *                            fuzziness is unnecessary.
 * @param {object} epsilon    Epsilon for comparison of numeric values.
 */

function fuzzy_test_computed_color(specified, computed, epsilon) {
  if (!computed) {
    test_computed_value("color", specified);
    return;
  }

  test_computed_value("color", specified, computed, undefined /* titleExtra */, {comparisonFunction: set_up_fuzzy_color_test(epsilon)});
}

/**
 * Test the computed value of a color property with some tolerance for numeric parameters.
 *
 * @param {string} property   A style property to test.
 * @param {string} specified  A specified value for the color.
 * @param {string} computed   The expected computed color. If omitted, defaults
 *                            to the default test_computed_value test, as
 *                            fuzziness is unnecessary.
 * @param {object} epsilon    Epsilon for comparison of numeric values.
 */

function fuzzy_test_computed_color_property(property, specified, computed, epsilon) {
  if (!computed) {
    test_computed_value(property, specified);
    return;
  }

  test_computed_value(property, specified, computed, undefined /* titleExtra */, {comparisonFunction: set_up_fuzzy_color_test(epsilon)});
}

/**
 * Test the parsed value of a color.
 *
 * @param {string} specified  A specified value for the property.
 * @param {string} parsed     The expected parsed color. If omitted, defaults
 *                            to the default test_valid_value test, as
 *                            fuzziness is unnecessary.
 * @param {object} epsilon    Epsilon for comparison of numeric values.
 */
function fuzzy_test_valid_color(specified, parsed, epsilon) {
  if (!parsed) {
    test_valid_value("color", specified);
    return;
  }

  test_valid_value("color", specified, parsed, {comparisonFunction: set_up_fuzzy_color_test(epsilon)});
}

/**
 * Test the parsed value of a color property.
 *
 * @param {string} property   A style property to test.
 * @param {string} specified  A specified value for the property.
 * @param {string} parsed     The expected parsed color. If omitted, defaults
 *                            to the default test_valid_value test, as
 *                            fuzziness is unnecessary.
 * @param {object} epsilon    Epsilon for comparison of numeric values.
 */
function fuzzy_test_valid_color_property(property, specified, parsed, epsilon) {
  if (!parsed) {
    test_valid_value(property, specified);
    return;
  }

  test_valid_value(property, specified, parsed, {comparisonFunction: set_up_fuzzy_color_test(epsilon)});
}

/**
 * Fuzzy color matcher for oklab color with optional transparency.
 * @param {string} actual    Observed color
 * @param {string} expected  What the color should be
 * @param {string} message   Error message to facilitate diagnostics
 */
function assert_oklab_color(actual, expected, message) {
  const paramMatch = '(\\-?\\d*\\.?\\d*)';
  const optAlphaMatch = '( \\/ (\\d*\\.?\\d*))?';
  const pattern =
      `oklab\\(${paramMatch} ${paramMatch} ${paramMatch}${optAlphaMatch}\\)`;
  const oklabRegex = new RegExp(pattern);
   let matches =
      expected.match(oklabRegex);
  assert_true(!!matches,
              `Expected value ${expected} not recognized as an oklab color`);

  const p0 = parseFloat(matches[1]);
  const p1 = parseFloat(matches[2]);
  const p2 = parseFloat(matches[3]);
  const alpha =
      (matches[5] !== undefined) ? parseFloat(matches[5]) : undefined;

  matches =
      actual.match(oklabRegex);
  assert_true(!!matches,
              `Actual value ${actual} not recognized as an oklab color`);

  const tolerance = 0.01;
  let colorMatch =
      Math.abs(parseFloat(matches[1]) - p0) <= tolerance &&
      Math.abs(parseFloat(matches[2]) - p1) <= tolerance &&
      Math.abs(parseFloat(matches[3]) - p2) <= tolerance;
  if (colorMatch) {
    if (alpha !== undefined) {
        colorMatch =
            matches[5] != undefined &&
            Math.abs(parseFloat(matches[5]) - alpha) <= tolerance;
    } else {
      colorMatch = matches[5] == undefined;
    }
  }
  assert_true(
      colorMatch,
      `expected: ${expected} actual ${actual} -- ${message}`);
}
