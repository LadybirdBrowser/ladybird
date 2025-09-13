function assert_is_unit(unit, result) {
  assert_class_string(result, 'CSSUnitValue',
    'relative lengths must compute to a CSSUnitValue');
  assert_equals(result.unit, unit, 'unit');
}

function assert_is_calc_sum(result) {
  assert_class_string(result, 'CSSMathSum',
    'specified calc must be a CSSMathSum');
}

function assert_is_equal_with_range_handling(input, result) {
  if (input instanceof CSSUnitValue && input.value < 0)
    assert_style_value_equals(result, new CSSMathSum(input));
  else
    assert_style_value_equals(result, input);
}

function assert_is_unsupported(result) {
  assert_class_string(result, 'CSSStyleValue');
}

const gCssWideKeywordsExamples = [
  {
    description: 'initial keyword',
    input: new CSSKeywordValue('initial')
  },
  {
    description: 'inherit keyword',
    input: new CSSKeywordValue('inherit')
  },
  {
    description: 'unset keyword',
    input: new CSSKeywordValue('unset')
  },
  {
    description: 'revert keyword',
    input: new CSSKeywordValue('revert')
  },
];

const gVarReferenceExamples = [
  {
    description: 'a var() reference',
    input: new CSSUnparsedValue([' ', new CSSVariableReferenceValue('--A')])
  },
];

const gTestSyntaxExamples = {
  '<length>': {
    description: 'a length',
    examples: [
      {
        description: "zero px",
        input: new CSSUnitValue(0, 'px')
      },
      {
        description: "a negative em",
        input: new CSSUnitValue(-3.14, 'em'),
        // 'ems' are relative units, so just check that it computes to px
        defaultComputed: (_, result) => assert_is_unit('px', result)
      },
      {
        description: "a positive cm",
        input: new CSSUnitValue(3.14, 'cm'),
        // 'cms' are relative units, so just check that it computes to px
        defaultComputed: (_, result) => assert_is_unit('px', result)
      },
      {
        description: "a calc length",
        input: new CSSMathSum(new CSSUnitValue(0, 'px'), new CSSUnitValue(0, 'em')),
        // Specified/computed calcs are usually simplified.
        // FIXME: Test this properly
        defaultSpecified: (_, result) => assert_is_calc_sum(result),
        defaultComputed: (_, result) => assert_is_unit('px', result)
      }
    ],
  },
  '<percentage>': {
    description: 'a percent',
    examples: [
      {
        description: "zero percent",
        input: new CSSUnitValue(0, 'percent')
      },
      {
        description: "a negative percent",
        input: new CSSUnitValue(-3.14, 'percent')
      },
      {
        description: "a positive percent",
        input: new CSSUnitValue(3.14, 'percent')
      },
      {
        description: "a calc percent",
        input: new CSSMathSum(new CSSUnitValue(0, 'percent'), new CSSUnitValue(0, 'percent')),
        // Specified/computed calcs are usually simplified.
        // FIXME: Test this properly
        defaultSpecified: (_, result) => assert_is_calc_sum(result),
        defaultComputed: (_, result) => assert_is_unit('percent', result)
      }
    ],
  },
  '<time>': {
    description: 'a time',
    examples: [
      {
        description: "zero seconds",
        input: new CSSUnitValue(0, 's')
      },
      {
        description: "negative milliseconds",
        input: new CSSUnitValue(-3.14, 'ms'),
        // Computed values use canonical units
        defaultComputed: (_, result) => assert_style_value_equals(result, new CSSUnitValue(-0.00314, 's'))
      },
      {
        description: "positive seconds",
        input: new CSSUnitValue(3.14, 's')
      },
      {
        description: "a calc time",
        input: new CSSMathSum(new CSSUnitValue(0, 's'), new CSSUnitValue(0, 'ms')),
        // Specified/computed calcs are usually simplified.
        // FIXME: Test this properly
        defaultSpecified: (_, result) => assert_is_calc_sum(result),
        defaultComputed: (_, result) => assert_is_unit('s', result)
      }
    ],
  },
  '<time>': {
    description: 'a time',
    examples: [
      {
        description: "zero seconds",
        input: new CSSUnitValue(0, 's')
      },
      {
        description: "negative milliseconds",
        input: new CSSUnitValue(-3.14, 'ms'),
        // Computed values use canonical units
        defaultComputed: (_, result) => assert_style_value_equals(result, new CSSUnitValue(-0.00314, 's'))
      },
      {
        description: "positive seconds",
        input: new CSSUnitValue(3.14, 's')
      },
      {
        description: "a calc time",
        input: new CSSMathSum(new CSSUnitValue(0, 's'), new CSSUnitValue(0, 'ms')),
        specifiedExpected: new CSSMathSum(new CSSUnitValue(0, 's'), new CSSUnitValue(0, 's')),
        defaultSpecified: (_, result) => assert_is_calc_sum(result),
        defaultComputed: (_, result) => assert_is_unit('s', result)
      }
    ],
  },
  '<angle>': {
    description: 'an angle',
    examples: [
      {
        description: "zero degrees",
        input: new CSSUnitValue(0, 'deg')
      },
      {
        description: "positive radians",
        input: new CSSUnitValue(3.14, 'rad'),
        // Computed values use canonical units
        defaultComputed: (_, result) => assert_style_value_equals(result, new CSSUnitValue(179.908752, 'deg'))
      },
      {
        description: "negative degrees",
        input: new CSSUnitValue(-3.14, 'deg')
      },
      {
        description: "a calc angle",
        input: new CSSMathSum(new CSSUnitValue(0, 'rad'), new CSSUnitValue(0, 'deg')),
        // Specified/computed calcs are usually simplified.
        // FIXME: Test this properly
        defaultSpecified: (_, result) => assert_is_calc_sum(result),
        defaultComputed: (_, result) => assert_is_unit('deg', result)
      }
    ],
  },
  '<flex>': {
    description: 'a flexible length',
    examples: [
      {
        description: "zero fractions",
        input: new CSSUnitValue(0, 'fr')
      },
      {
        description: "one fraction",
        input: new CSSUnitValue(1, 'fr')
      },
      // TODO(https://github.com/w3c/css-houdini-drafts/issues/734):
      // Add calc tests involving 'fr' when that is spec'd in CSS.
    ],
  },
  '<negative-flex>': {
    description: 'a flexible length',
    examples: [
      {
        description: "negative fraction",
        input: new CSSUnitValue(-3.14, 'fr')
      },
    ],
  },
  '<number>': {
    description: 'a number',
    examples: [
      {
        description: 'the number zero',
        input: new CSSUnitValue(0, 'number')
      },
      {
        description: 'a negative number',
        input: new CSSUnitValue(-3.14, 'number')
      },
      {
        description: 'a positive number',
        input: new CSSUnitValue(3.14, 'number')
      },
      {
        description: "a calc number",
        input: new CSSMathSum(new CSSUnitValue(2, 'number'), new CSSUnitValue(3, 'number')),
        defaultSpecified: (_, result) => assert_is_calc_sum(result),
        defaultComputed: (_, result) => {
          assert_style_value_equals(result, new CSSUnitValue(5, 'number'));
        }
      }
    ],
  },
  '<url>': {
    description: 'a URL',
    examples: [
      // TODO(https://github.com/w3c/css-houdini-drafts/issues/716):
      // We can't test this until CSSURLValue is spec'd.
    ],
  },
  '<transform>': {
    description: 'a transform',
    examples: [
      {
        description: 'a transform containing percents',
        input: new CSSTransformValue([
          new CSSTranslate(
            new CSSUnitValue(50, 'percent'),
            new CSSUnitValue(50, 'percent'),
          )
        ]),
      },
      {
        description: 'a transform containing relative values',
        input: new CSSTransformValue([
          new CSSPerspective(new CSSUnitValue(10, 'em'))
        ]),
        defaultComputed: (_, result) => {
          // Relative units compute to absolute.
          assert_class_string(result, 'CSSTransformValue',
            'Result must be a CSSTransformValue');
          assert_class_string(result[0], 'CSSPerspective',
            'First component must be a CSSTransformValue');
          assert_is_unit('px', result[0].length);
        }
      },
      {
        description: 'a transform containing all the transform components',
        input: new CSSTransformValue([
          new CSSTranslate(
            new CSSUnitValue(0, 'px'),
            new CSSUnitValue(1, 'px'),
            new CSSUnitValue(2, 'px'),
          ),
          new CSSTranslate(
            new CSSUnitValue(0, 'px'),
            new CSSUnitValue(1, 'px'),
          ),
          new CSSRotate(1, 2, 3, new CSSUnitValue(45, 'deg')),
          new CSSRotate(new CSSUnitValue(45, 'deg')),
          new CSSScale(1, 2, 3),
          new CSSScale(1, 2),
          new CSSSkew(new CSSUnitValue(1, 'deg'), new CSSUnitValue(1, 'deg')),
          new CSSSkewX(new CSSUnitValue(1, 'deg')),
          new CSSSkewY(new CSSUnitValue(45, 'deg')),
          new CSSPerspective(new CSSUnitValue(1, 'px')),
          new CSSMatrixComponent(new DOMMatrixReadOnly(
            [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16])
          ),
          new CSSMatrixComponent(new DOMMatrixReadOnly([1, 2, 3, 4, 5, 6])),
        ]),
      }
    ],
  },
};

// Test setting a value in a style map and then getting it from the inline and
// computed styles.
function testPropertyValid(propertyName, examples, specified, computed, description) {
  for (const example of examples) {
    test(t => {
      let element = createDivWithStyle(t);

      element.attributeStyleMap.set(propertyName, example.input);

      // specified style
      const specifiedResult = element.attributeStyleMap.get(propertyName);
      assert_not_equals(specifiedResult, null,
        'Specified value must not be null');
      assert_true(specifiedResult instanceof CSSStyleValue,
        'Specified value must be a CSSStyleValue');

      if (specified || example.defaultSpecified) {
        (specified || example.defaultSpecified)(example.specifiedExpected || example.input, specifiedResult);
      } else {
        assert_style_value_equals(specifiedResult, example.input,
          `Setting ${example.description} and getting its specified value`);
      }

      // computed style
      const computedResult = element.computedStyleMap().get(propertyName);
      assert_not_equals(computedResult, null,
        'Computed value must not be null');
      assert_true(computedResult instanceof CSSStyleValue,
        'Computed value must be a CSSStyleValue');

      if (computed || example.defaultComputed) {
        (computed || example.defaultComputed)(example.input, computedResult);
      } else {
        assert_style_value_equals(computedResult, example.input,
          `Setting ${example.description} and getting its computed value`);
      }
    }, `Can set '${propertyName}' to ${description}: ${example.input}`);
  }
}

// We have to special case CSSImageValue as they cannot be created with a
// constructor and are completely opaque.
function testIsImageValidForProperty(propertyName) {
  test(t => {
    let element1 = createDivWithStyle(t, `${propertyName}: url("/media/1x1-green.png")`);
    let element2 = createDivWithStyle(t);

    const result = element1.attributeStyleMap.get(propertyName);
    assert_not_equals(result, null, 'Image value must not be null');
    assert_class_string(result, 'CSSImageValue',
      'Image value must be a CSSImageValue');

    element2.attributeStyleMap.set(propertyName, result);
    assert_equals(element2.style[propertyName], element1.style[propertyName],
      'Image value can be set on different element');
  }, `Can set '${propertyName}' to an image`);
}

// Test that styleMap.set throws for invalid values
function testPropertyInvalid(propertyName, examples, description) {
  for (const example of examples) {
    test(t => {
      let styleMap = createInlineStyleMap(t);
      assert_throws_js(TypeError, () => styleMap.set(propertyName, example.input));
    }, `Setting '${propertyName}' to ${description}: ${example.input} throws TypeError`);
  }
}

// Test that styleMap.get/.set roundtrips correctly for unsupported values.
function testUnsupportedValue(propertyName, cssText) {
  test(t => {
    let element1 = createDivWithStyle(t);
    let element2 = createDivWithStyle(t);

    element1.style[propertyName] = cssText;
    const result = element1.attributeStyleMap.get(propertyName);
    assert_not_equals(result, null,
      'Unsupported value must not be null');
    assert_class_string(result, 'CSSStyleValue',
      'Unsupported value must be a CSSStyleValue and not one of its subclasses');

    element2.attributeStyleMap.set(propertyName, result);
    assert_equals(element2.style[propertyName], element1.style[propertyName],
      'Unsupported value can be set on different element');

    const resultAll = element2.attributeStyleMap.getAll(propertyName);
    assert_style_value_equals(resultAll[0], result,
      `getAll() with single unsupported value returns single-item list ` +
      `with same result as get()`);
  }, `'${propertyName}' does not support '${cssText}'`);
}

function createKeywordExample(keyword) {
  return {
    description: `the '${keyword}' keyword`,
    examples: [ { input: new CSSKeywordValue(keyword) } ]
  };
}

// Run a battery of StylePropertyMap tests on |propertyName|.
// Second argument is a list of test cases. A test case has the form:
//
// {
//   syntax: "<length>",
//   specified: /* a callback */ (optional)
//   computed: /* a callback */ (optional)
// }
//
// If a callback is passed to |specified|, then the callback will be passed
// two arguments:
// 1. The input test case
// 2. The result of calling get() on the inline style map (specified values).
//
// The callback should check if the result is expected using assert_* functions.
// If no callback is passed, then we assert that the result is the same as
// the input.
//
// Same goes for |computed|, but with the computed style map (computed values).
//
// FIXME: The reason we pass argument #2 is that it's sometimes difficult to
// compute exactly what the expected result should be (e.g. browser-specific
// values). Once we can do that, we can remove argument #2 and just return
// the expected result.
function runPropertyTests(propertyName, testCases) {
  let syntaxTested = new Set();

  // Every property should at least support CSS-wide keywords.
  testPropertyValid(propertyName,
    gCssWideKeywordsExamples,
    null, // should be as specified
    () => {}, // could be anything
    'CSS-wide keywords');

  // Every property should support values containing var() references.
  testPropertyValid(propertyName,
    gVarReferenceExamples,
    null, // should be as specified
    () => {}, // could compute to anything
    'var() references');

  for (const testCase of testCases) {
    // <image> is a special case
    if (testCase.syntax === '<image>') {
      testIsImageValidForProperty(propertyName);
      continue;
    }

    // Retrieve test examples for this test case's syntax. If the syntax
    // looks like a keyword, then create an example on the fly.
    const syntaxExamples = testCase.syntax.toLowerCase().match(/^[a-z0-9\-]+$/) ?
      createKeywordExample(testCase.syntax) :
      gTestSyntaxExamples[testCase.syntax];

    if (!syntaxExamples)
      throw new Error(`'${testCase.syntax}' is not a valid CSS component`);

    testPropertyValid(propertyName,
      syntaxExamples.examples,
      testCase.specified,
      testCase.computed,
      syntaxExamples.description);

    syntaxTested.add(testCase.syntax);
  }

  // Also test that styleMap.set rejects invalid CSSStyleValues.
  for (const [syntax, syntaxExamples] of Object.entries(gTestSyntaxExamples)) {
    if (!syntaxTested.has(syntax)) {
      testPropertyInvalid(propertyName,
        syntaxExamples.examples,
        syntaxExamples.description);
    }
  }
}

// Same as runPropertyTests but for list-valued properties.
function runListValuedPropertyTests(propertyName, testCases) {
  // TODO(https://crbug.com/545318): Run list-valued tests as well.
  runPropertyTests(propertyName, testCases);
}

// Check that |propertyName| doesn't "support" examples in |testExamples|.
// |testExamples| is a list of CSS string values. An "unsupported" value
// doesn't have a corresponding Typed OM representation. It normalizes as
// the base CSSStyleValue.
function runUnsupportedPropertyTests(propertyName, testExamples) {
  for (const cssText of testExamples) {
    testUnsupportedValue(propertyName, cssText);
  }
}
