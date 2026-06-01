'use strict';

function runAnimationTypeTest(gCSSProperties, testType) {
  for (const property in gCSSProperties) {
    if (!isSupported(property)) {
      continue;
    }

    const setupFunction = gCSSProperties[property].setup;
    for (const animationType of gCSSProperties[property].types) {
      let typeObject;
      let animationTypeString;
      if (typeof animationType === 'string') {
        typeObject = types[animationType];
        animationTypeString = animationType;
      } else if (typeof animationType === 'object' &&
                 animationType.type && typeof animationType.type === 'string') {
        typeObject = types[animationType.type];
        animationTypeString = animationType.type;
      }

      // First, test that the animation type object has 'testAccumulation', or
      // 'testAddition', or 'testInterpolation'.
      // We use test() function here so that we can continue the remainder tests
      // even if this test fails.
      test(t => {
        assert_own_property(typeObject, testType, animationTypeString +
                            ` should have ${testType} property`);
        assert_equals(typeof typeObject[testType], 'function',
                      `${testType} method should be a function`);
      }, `${property} (type: ${animationTypeString}) has ${testType} function`);

      if (typeObject[testType] && typeof typeObject[testType] === 'function') {
        typeObject[testType](property, setupFunction, animationType.options);
      }
    }
  }
}
