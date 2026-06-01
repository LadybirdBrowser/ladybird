'use strict';

const expected = values => {
    // Some properties, such as line-height, report computed values which differ
    // from the keyframe values. To support this, we allow optional values to specify
    // explicit "from" and "to" values as additional keyframe values.
    const [ from, to ] = values;
    return [ values[2] ?? from, values[3] ?? to ];
};

const discreteType = {
  testInterpolation: (property, setup, options) => {
    for (const keyframes of options) {
      const [ from, to ] = keyframes;
      const [ expectedFrom, expectedTo ] = expected(keyframes);
      test(t => {
        const idlName = propertyToIDL(property);
        const target = createTestElement(t, setup);
        const animation = target.animate({ [idlName]: [from, to] },
                                         { duration: 1000, fill: 'both' });
        testAnimationSamples(animation, idlName,
                             [{ time: 0,    expected: expectedFrom.toLowerCase() },
                              { time: 499,  expected: expectedFrom.toLowerCase() },
                              { time: 500,  expected: expectedTo.toLowerCase() },
                              { time: 1000, expected: expectedTo.toLowerCase() }]);
      }, `${property} uses discrete animation when animating between`
         + ` "${from}" and "${to}" with linear easing`);

      test(t => {
        // Easing: http://cubic-bezier.com/#.68,0,1,.01
        // With this curve, we don't reach the 50% point until about 95% of
        // the time has expired.
        const idlName = propertyToIDL(property);
        const keyframes = {};
        const target = createTestElement(t, setup);
        const animation = target.animate(
          { [idlName]: [from, to] },
          {
            duration: 1000,
            fill: 'both',
            easing: 'cubic-bezier(0.68,0,1,0.01)',
          }
        );
        testAnimationSamples(animation, idlName,
                             [{ time: 0,    expected: expectedFrom.toLowerCase() },
                              { time: 940,  expected: expectedFrom.toLowerCase() },
                              { time: 960,  expected: expectedTo.toLowerCase() }]);
      }, `${property} uses discrete animation when animating between`
         + ` "${from}" and "${to}" with effect easing`);

      test(t => {
        // Easing: http://cubic-bezier.com/#.68,0,1,.01
        // With this curve, we don't reach the 50% point until about 95% of
        // the time has expired.
        const idlName = propertyToIDL(property);
        const target = createTestElement(t, setup);
        const animation = target.animate(
          {
            [idlName]: [from, to],
            easing: 'cubic-bezier(0.68,0,1,0.01)',
          },
          { duration: 1000, fill: 'both' }
        );
        testAnimationSamples(animation, idlName,
                             [{ time: 0,    expected: expectedFrom.toLowerCase() },
                              { time: 940,  expected: expectedFrom.toLowerCase() },
                              { time: 960,  expected: expectedTo.toLowerCase() }]);
      }, `${property} uses discrete animation when animating between`
         + ` "${from}" and "${to}" with keyframe easing`);
    }
  },

  testAdditionOrAccumulation: (property, setup, options, composite) => {
    for (const keyframes of options) {
      const [ from, to ] = keyframes;
      const [ expectedFrom, expectedTo ] = expected(keyframes);
      test(t => {
        const idlName = propertyToIDL(property);
        const target = createTestElement(t, setup);
        target.animate({ [idlName]: [from, from] }, 1000);
        const animation = target.animate(
          { [idlName]: [to, to] },
          { duration: 1000, composite }
        );
        testAnimationSamples(animation, idlName,
                             [{ time: 0, expected: expectedTo.toLowerCase() }]);
      }, `${property}: "${to}" onto "${from}"`);

      test(t => {
        const idlName = propertyToIDL(property);
        const target = createTestElement(t, setup);
        target.animate({ [idlName]: [to, to] }, 1000);
        const animation = target.animate(
          { [idlName]: [from, from] },
          { duration: 1000, composite }
        );
        testAnimationSamples(animation, idlName,
                             [{ time: 0, expected: expectedFrom.toLowerCase() }]);
      }, `${property}: "${from}" onto "${to}"`);
    }
  },

  testAddition: function(property, setup, options) {
    this.testAdditionOrAccumulation(property, setup, options, 'add');
  },

  testAccumulation: function(property, setup, options) {
    this.testAdditionOrAccumulation(property, setup, options, 'accumulate');
  },
};

const lengthType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['10px', '50px'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: '30px' }]);
    }, `${property} supports animating as a length`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['1rem', '5rem'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: '30px' }]);
    }, `${property} supports animating as a length of rem`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '10px';
      const animation = target.animate(
        { [idlName]: ['10px', '50px'] },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName, [{ time: 0, expected: '20px' }]);
    }, `${property}: length`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '1rem';
      const animation = target.animate(
        { [idlName]: ['1rem', '5rem'] },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName, [{ time: 0, expected: '20px' }]);
    }, `${property}: length of rem`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const lengthPairType = {
  testInterpolation: (property, setup, serializesToSingleValue = true) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]: ['10px 10px', '50px 50px'] },
        { duration: 1000, fill: 'both' }
      );

      let expected = '30px 30px';
      if (serializesToSingleValue) {
        expected = '30px';
      }

      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected }]);
    }, `${property} supports animating as a length pair`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]: ['1rem 1rem', '5rem 5rem'] },
        { duration: 1000, fill: 'both' }
      );

      let expected = '30px 30px';
      if (serializesToSingleValue) {
        expected = '30px';
      }

      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected }]);
    }, `${property} supports animating as a length pair of rem`);
  },

  testAdditionOrAccumulation: (property, setup, composite, serializesToSingleValue = true) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '10px 10px';
      const animation = target.animate(
        { [idlName]: ['10px 10px', '50px 50px'] },
        { duration: 1000, composite }
      );

      let expected = '20px 20px';
      if (serializesToSingleValue) {
        expected = '20px';
      }

      testAnimationSamples(
        animation,
        idlName,
        [{ time: 0, expected }]
      );
    }, `${property}: length pair`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '1rem 1rem';
      const animation = target.animate(
        { [idlName]: ['1rem 1rem', '5rem 5rem'] },
        { duration: 1000, composite }
      );


      let expected = '20px 20px';
      if (serializesToSingleValue) {
        expected = '20px';
      }

      testAnimationSamples(
        animation,
        idlName,
        [{ time: 0, expected }]
      );
    }, `${property}: length pair of rem`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const percentageType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['10%', '50%'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: '30%' }]);
    }, `${property} supports animating as a percentage`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '60%';
      const animation = target.animate(
        { [idlName]: ['70%', '100%'] },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName, [{ time: 0, expected: '130%' }]);
    }, `${property}: percentage`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const integerType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: [-2, 2] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: '0' }]);
    }, `${property} supports animating as an integer`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = -1;
      const animation = target.animate(
        { [idlName]: [-2, 2] },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName,
                           [{ time: 0,    expected: '-3' }]);
    }, `${property}: integer`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const positiveIntegerType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: [1, 3] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [ { time: 500,  expected: '2' } ]);
    }, `${property} supports animating as a positive integer`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 1;
      const animation = target.animate(
        { [idlName]: [2, 5] },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName,
                           [{ time: 0,    expected: '3' }]);
    }, `${property}: positive integer`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const lengthPercentageOrCalcType = {
  testInterpolation: (property, setup) => {
    lengthType.testInterpolation(property, setup);
    percentageType.testInterpolation(property, setup);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['10px', '20%'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'calc(10% + 5px)' }]);
    }, `${property} supports animating as combination units "px" and "%"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['10%', '2em'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'calc(5% + 10px)' }]);
    }, `${property} supports animating as combination units "%" and "em"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['1em', '2rem'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: '15px' }]);
    }, `${property} supports animating as combination units "em" and "rem"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]: ['10px', 'calc(1em + 20%)'] },
        { duration: 1000, fill: 'both' }
      );
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'calc(10% + 10px)' }]);
    }, `${property} supports animating as combination units "px" and "calc"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]: ['calc(10px + 10%)', 'calc(1em + 1rem + 20%)'] },
        { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,
                              expected: 'calc(15% + 15px)' }]);
    }, `${property} supports animating as a calc`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    lengthType.testAddition(property, setup);
    percentageType.testAddition(property, setup);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '10px';
      const animation = target.animate({ [idlName]: ['10%', '50%'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0, expected: 'calc(10% + 10px)' }]);
    }, `${property}: units "%" onto "px"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '10%';
      const animation = target.animate({ [idlName]: ['10px', '50px'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0, expected: 'calc(10% + 10px)' }]);
    }, `${property}: units "px" onto "%"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '10%';
      const animation = target.animate({ [idlName]: ['2rem', '5rem'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0, expected: 'calc(10% + 20px)' }]);
    }, `${property}: units "rem" onto "%"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2rem';
      const animation = target.animate({ [idlName]: ['10%', '50%'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0, expected: 'calc(10% + 20px)' }]);
    }, `${property}: units "%" onto "rem"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2em';
      const animation = target.animate({ [idlName]: ['2rem', '5rem'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName, [{ time: 0, expected: '40px' }]);
    }, `${property}: units "rem" onto "em"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2rem';
      const animation = target.animate({ [idlName]: ['2em', '5em'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName, [{ time: 0, expected: '40px' }]);
    }, `${property}: units "em" onto "rem"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '10px';
      const animation = target.animate({ [idlName]: ['calc(2em + 20%)',
                                                   'calc(5rem + 50%)'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0, expected: 'calc(20% + 30px)' }]);
    }, `${property}: units "calc" onto "px"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'calc(10px + 10%)';
      const animation = target.animate({ [idlName]: ['calc(20px + 20%)',
                                                   'calc(2em + 3rem + 40%)'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0, expected: 'calc(30% + 30px)' }]);
    }, `${property}: calc`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const positiveNumberType = {
  testInterpolation: (property, setup, expectedUnit='') => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: [1.1, 1.5] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: '1.3' + expectedUnit }]);
    }, `${property} supports animating as a positive number`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 1.1;
      const animation = target.animate({ [idlName]: [1.1, 1.5] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName, [{ time: 0, expected: '2.2' }]);
    }, `${property}: positive number`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

// Test using float values in the range [0, 1]
const opacityType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: [0.3, 0.8] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: '0.55' }]);
    }, `${property} supports animating as a [0, 1] number`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 0.3;
      const animation = target.animate({ [idlName]: [0.3, 0.8] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName, [{ time: 0, expected: '0.6' }]);
    }, `${property}: [0, 1] number`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 0.8;
      const animation = target.animate({ [idlName]: [0.3, 0.8] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName, [{ time: 0, expected: '1' }]);
    }, `${property}: [0, 1] number (clamped)`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const visibilityType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['visible', 'hidden'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,    expected: 'visible' },
                            { time: 999,  expected: 'visible' },
                            { time: 1000, expected: 'hidden' }]);
    }, `${property} uses visibility animation when animating`
       + ' from "visible" to "hidden"');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['hidden', 'visible'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,    expected: 'hidden' },
                            { time: 1,    expected: 'visible' },
                            { time: 1000, expected: 'visible' }]);
    }, `${property} uses visibility animation when animating`
       + ' from "hidden" to "visible"');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['hidden', 'collapse'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,    expected: 'hidden' },
                            { time: 499,  expected: 'hidden' },
                            { time: 500,  expected: 'collapse' },
                            { time: 1000, expected: 'collapse' }]);
    }, `${property} uses visibility animation when animating`
       + ' from "hidden" to "collapse"');

    test(t => {
      // Easing: http://cubic-bezier.com/#.68,-.55,.26,1.55
      // With this curve, the value is less than 0 till about 34%
      // also more than 1 since about 63%
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['visible', 'hidden'] },
                       { duration: 1000, fill: 'both',
                         easing: 'cubic-bezier(0.68, -0.55, 0.26, 1.55)' });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,    expected: 'visible' },
                            { time: 1,    expected: 'visible' },
                            { time: 330,  expected: 'visible' },
                            { time: 340,  expected: 'visible' },
                            { time: 620,  expected: 'visible' },
                            { time: 630,  expected: 'hidden' },
                            { time: 1000, expected: 'hidden' }]);
    }, `${property} uses visibility animation when animating`
       + ' from "visible" to "hidden" with easeInOutBack easing');
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'visible';
      const animation = target.animate({ [idlName]: ['visible', 'hidden'] },
                                       { duration: 1000, fill: 'both',
                                         composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,    expected: 'visible' },
                            { time: 1000, expected: 'hidden' }]);
    }, `${property}: onto "visible"`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'hidden';
      const animation = target.animate({ [idlName]: ['hidden', 'visible'] },
                                       { duration: 1000, fill: 'both',
                                         composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,    expected: 'hidden' },
                            { time: 1000, expected: 'visible' }]);
    }, `${property}: onto "hidden"`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const colorType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['rgb(255, 0, 0)',
                                                     'rgb(0, 0, 255)'] },
                                       1000);
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'rgb(128, 0, 128)' }]);
    }, `${property} supports animating as color of rgb()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['#ff0000', '#0000ff'] },
                                       1000);
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'rgb(128, 0, 128)' }]);
    }, `${property} supports animating as color of #RGB`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['hsl(0,   100%, 50%)',
                                                     'hsl(240, 100%, 50%)'] },
                                       1000);
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'rgb(128, 0, 128)' }]);
    }, `${property} supports animating as color of hsl()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]: ['#ff000066', '#0000ffcc'] },
        1000
      );
      // R: 255 * (0.4 * 0.5) / 0.6 = 85
      // B: 255 * (0.8 * 0.5) / 0.6 = 170
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'rgba(85, 0, 170, 0.6)' }]);
    }, `${property} supports animating as color of #RGBa`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['rgba(255, 0, 0, 0.4)', 'rgba(0, 0, 255, 0.8)'],
        },
        1000
      );
      testAnimationSamples(animation, idlName,      // Same as above.
                           [{ time: 500,  expected: 'rgba(85, 0, 170, 0.6)' }]);
    }, `${property} supports animating as color of rgba()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['hsla(0,   100%, 50%, 0.4)', 'hsla(240, 100%, 50%, 0.8)'],
        },
        1000
      );
      testAnimationSamples(animation, idlName,      // Same as above.
                           [{ time: 500,  expected: 'rgba(85, 0, 170, 0.6)' }]);
    }, `${property} supports animating as color of hsla()`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128)';
      const animation = target.animate(
        {
          [idlName]: ['rgb(255, 0, 0)', 'rgb(0, 0, 255)'],
        },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName,
                           [{ time: 0,   expected: 'rgb(255, 128, 128)' },
                            // The value at 50% is interpolated
                            // from 'rgb(128+255, 128, 128)'
                            // to   'rgb(128,     128, 128+255)'.
                            { time: 500, expected: 'rgb(255, 128, 255)' }]);
    }, `${property} supports animating as color of rgb() with overflowed `
       + ' from and to values');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128)';
      const animation = target.animate({ [idlName]: ['#ff0000', '#0000ff'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,  expected: 'rgb(255, 128, 128)' }]);
    }, `${property} supports animating as color of #RGB`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128)';
      const animation = target.animate({ [idlName]: ['hsl(0,   100%, 50%)',
                                                     'hsl(240, 100%, 50%)'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,  expected: 'rgb(255, 128, 128)' }]);
    }, `${property} supports animating as color of hsl()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128)';
      const animation = target.animate(
        { [idlName]: ['#ff000066', '#0000ffcc'] },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName,
                           [{ time: 0,  expected: 'rgb(230, 128, 128)' }]);
    }, `${property} supports animating as color of #RGBa`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128)';
      const animation = target.animate({ [idlName]: ['rgba(255, 0, 0, 0.4)',
                                                     'rgba(0, 0, 255, 0.8)'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,      // Same as above.
                           [{ time: 0,  expected: 'rgb(230, 128, 128)' }]);
    }, `${property} supports animating as color of rgba()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128)';
      const animation = target.animate(
        {
          [idlName]: ['hsla(0,   100%, 50%, 0.4)', 'hsla(240, 100%, 50%, 0.8)'],
        },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName,      // Same as above.
                           [{ time: 0,  expected: 'rgb(230, 128, 128)' }]);
    }, `${property} supports animating as color of hsla()`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const colorPairType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['rgb(255, 0, 0) rgb(0, 0, 255)',
                                                     'rgb(0, 0, 255) rgb(0, 255, 0)'] },
                                       1000);
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'rgb(128, 0, 128) rgb(0, 128, 128)' }]);
    }, `${property} supports animating as color pair of rgb()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['#ff0000 #00ff00', '#0000ff #ff0000'] },
                                       1000);
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'rgb(128, 0, 128) rgb(128, 128, 0)' }]);
    }, `${property} supports animating as color pair of #RGB`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['hsl(0,   100%, 50%) hsl(0,   100%, 50%)',
                                                     'hsl(240, 100%, 50%) hsl(360, 100%, 50%)'] },
                                       1000);
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'rgb(128, 0, 128) rgb(255, 0, 0)' }]);
    }, `${property} supports animating as color pair of hsl()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]: ['#ff000066 #ff000066', '#0000ffcc #00ff00cc'] },
        1000
      );
      // R: 255 * (0.4 * 0.5) / 0.6 = 85
      // B: 255 * (0.8 * 0.5) / 0.6 = 170
      testAnimationSamples(animation, idlName,
                           [{ time: 500,  expected: 'rgba(85, 0, 170, 0.6) rgba(85, 170, 0, 0.6)' }]);
    }, `${property} supports animating as color pair of #RGBa`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['rgba(255, 0, 0, 0.4) rgba(255, 0, 0, 0.4)', 'rgba(0, 0, 255, 0.8) rgba(0, 255, 0, 0.8)'],
        },
        1000
      );
      testAnimationSamples(animation, idlName,      // Same as above.
                           [{ time: 500,  expected: 'rgba(85, 0, 170, 0.6) rgba(85, 170, 0, 0.6)' }]);
    }, `${property} supports animating as color pair of rgba()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['hsla(0,   100%, 50%, 0.4) hsla(0,   100%, 50%, 0.4)', 'hsla(240, 100%, 50%, 0.8) hsla(360, 100%, 50%, 0.8)'],
        },
        1000
      );
      testAnimationSamples(animation, idlName,      // Same as above.
                           [{ time: 500,  expected: 'rgba(85, 0, 170, 0.6) rgba(255, 0, 0, 0.6)' }]);
    }, `${property} supports animating as color pair of hsla()`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128) rgb(0, 0, 0)';
      const animation = target.animate(
        {
          [idlName]: ['rgb(255, 0, 0) rgb(0, 0, 255)', 'rgb(0, 0, 255) rgb(255, 0, 0)']
        },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName,
                           [{ time: 0,   expected: 'rgb(255, 128, 128) rgb(0, 0, 255)' },
                            { time: 500, expected: 'rgb(255, 128, 255) rgb(128, 0, 128)' }]);
    }, `${property} supports animating as color pair of rgb() with overflowed `
       + ' from and to values');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128) rgb(0, 0, 0)';
      const animation = target.animate({ [idlName]: ['#ff0000 #0000ff', '#0000ff #ff0000'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,  expected: 'rgb(255, 128, 128) rgb(0, 0, 255)' }]);
    }, `${property} supports animating as color pair of #RGB`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128) rgb(0, 0, 0)';
      const animation = target.animate({ [idlName]: ['hsl(0,   100%, 50%) hsl(0,   100%, 50%)',
                                                     'hsl(240, 100%, 50%) hsl(360, 100%, 50%)'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,  expected: 'rgb(255, 128, 128) rgb(255, 0, 0)' }]);
    }, `${property} supports animating as color pair of hsl()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128) rgb(0, 0, 0)';
      const animation = target.animate(
        { [idlName]: ['#ff000066 #ff000066', '#0000ffcc #00ff00cc'] },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName,
                           [{ time: 0,  expected: 'rgb(230, 128, 128) rgb(102, 0, 0)' }]);
    }, `${property} supports animating as color pair of #RGBa`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128) rgb(0, 0, 0)';
      const animation = target.animate({ [idlName]: ['rgba(255, 0, 0, 0.4) rgba(0, 255, 0, 0.4)',
                                                     'rgba(0, 0, 255, 0.8) rgba(255, 0, 0, 0.8)'] },
                                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,      // Same as above.
                           [{ time: 0,  expected: 'rgb(230, 128, 128) rgb(0, 102, 0)' }]);
    }, `${property} supports animating as color pair of rgba()`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(128, 128, 128) rgb(0, 0, 0)';
      const animation = target.animate(
        {
          [idlName]: ['hsla(0,   100%, 50%, 0.4) hsla(0,   100%, 50%, 0.4)', 'hsla(240, 100%, 50%, 0.8) hsla(360, 100%, 50%, 0.8)'],
        },
        { duration: 1000, composite }
      );
      testAnimationSamples(animation, idlName,      // Same as above.
                           [{ time: 0,  expected: 'rgb(230, 128, 128) rgb(102, 0, 0)' }]);
    }, `${property} supports animating as color pair of hsla()`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const transformListType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['translate(200px, -200px)', 'translate(400px, 400px)'],
        },
        1000
      );
      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500,  expected: [ 1, 0, 0, 1, 300, 100 ] }]);
    }, `${property}: translate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['rotate(45deg)', 'rotate(135deg)'],
        },
        1000
      );

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500,  expected: [ Math.cos(Math.PI / 2),
                                   Math.sin(Math.PI / 2),
                                  -Math.sin(Math.PI / 2),
                                   Math.cos(Math.PI / 2),
                                   0, 0] }]);
    }, `${property}: rotate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['scale(3)', 'scale(5)'] },
                                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500,  expected: [ 4, 0, 0, 4, 0, 0 ] }]);
    }, `${property}: scale`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['skew(30deg, 60deg)',
                                                     'skew(60deg, 30deg)'] },
                                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500,  expected: [ 1, Math.tan(Math.PI / 4),
                                   Math.tan(Math.PI / 4), 1,
                                   0, 0] }]);
    }, `${property}: skew`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['translateX(100px) rotate(45deg)',
                                     'translateX(200px) rotate(135deg)'] },
                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500,  expected: [ Math.cos(Math.PI / 2),
                                   Math.sin(Math.PI / 2),
                                  -Math.sin(Math.PI / 2),
                                   Math.cos(Math.PI / 2),
                                   150, 0 ] }]);
    }, `${property}: rotate and translate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['rotate(45deg) translateX(100px)',
                                     'rotate(135deg) translateX(200px)'] },
                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500, expected: [ Math.cos(Math.PI / 2),
                                  Math.sin(Math.PI / 2),
                                 -Math.sin(Math.PI / 2),
                                  Math.cos(Math.PI / 2),
                                  150 * Math.cos(Math.PI / 2),
                                  150 * Math.sin(Math.PI / 2) ] }]);
    }, `${property}: translate and rotate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['rotate(0deg)',
                                     'rotate(1080deg) translateX(100px)'] },
                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500, expected: [ -1, 0, 0, -1, -50, 0 ] }]);
    }, `${property}: extend shorter list (from)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['rotate(0deg) translateX(100px)',
                                     'rotate(1080deg)'] },
                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500, expected: [ -1, 0, 0, -1, -50, 0 ] }]);
    }, `${property}: extend shorter list (to)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =              // matrix(0, 1, -1, 0, 0, 100)
        target.animate({ [idlName]: ['rotate(90deg) translateX(100px)',
                                     // matrix(-1, 0, 0, -1, 200, 0)
                                     'translateX(200px) rotate(180deg)'] },
                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500, expected: [ Math.cos(Math.PI * 3 / 4),
                                  Math.sin(Math.PI * 3 / 4),
                                 -Math.sin(Math.PI * 3 / 4),
                                  Math.cos(Math.PI * 3 / 4),
                                  100, 50 ] }]);
    }, `${property}: mismatch order of translate and rotate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =               // Same matrices as above.
        target.animate({ [idlName]: [ 'matrix(0, 1, -1, 0, 0, 100)',
                                      'matrix(-1, 0, 0, -1, 200, 0)' ] },
                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500, expected: [ Math.cos(Math.PI * 3 / 4),
                                  Math.sin(Math.PI * 3 / 4),
                                 -Math.sin(Math.PI * 3 / 4),
                                  Math.cos(Math.PI * 3 / 4),
                                  100, 50 ] }]);
    }, `${property}: matrix`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rotate3d(1, 1, 0, 0deg)',
                                      'rotate3d(1, 1, 0, 90deg)'] },
                       1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500, expected: rotate3dToMatrix(1, 1, 0, Math.PI / 4) }]);
    }, `${property}: rotate3d`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      // To calculate expected matrices easily, generate input matrices from
      // rotate3d.
      const from = rotate3dToMatrix3d(1, 1, 0, Math.PI / 4);
      const to = rotate3dToMatrix3d(1, 1, 0, Math.PI * 3 / 4);
      const animation = target.animate({ [idlName]: [ from, to ] }, 1000);

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500, expected: rotate3dToMatrix(1, 1, 0, Math.PI * 2 / 4) }]);
    }, `${property}: matrix3d`);

    // This test aims for forcing the two mismatched transforms to be
    // decomposed into matrix3d before interpolation. Therefore, we not only
    // test the interpolation, but also test the 3D matrix decomposition.
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: [
            'scale(0.3)',
            // scale(0.5) translateZ(1px)
            'matrix3d(0.5, 0, 0, 0, 0, 0.5, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1)',
          ],
        },
        1000
      );

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500,  expected: [ 0.4, 0,   0,   0,
                                   0,   0.4, 0,   0,
                                   0,   0,   1,   0,
                                   0,   0,   0.5, 1] }]);
    }, `${property}: mismatched 3D transforms`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['rotateY(60deg)', 'none' ] }, 1000);

      testAnimationSampleMatrices(animation, idlName,
                   // rotateY(30deg) == rotate3D(0, 1, 0, 30deg)
        [{ time: 500, expected: rotate3dToMatrix(0, 1, 0, Math.PI / 6) }]);
    }, `${property}: rotateY`);

    // Following tests aim for test the fallback discrete interpolation behavior
    // for non-invertible matrices. The non-invertible matrix that we use is the
    // singular matrix, matrix(1, 1, 0, 0, 0, 100).
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['matrix(-1, 0, 0, -1, 200, 0)',
                                     'matrix( 1, 1, 0,  0, 0, 100)'] },
                       { duration: 1000, fill: 'both' });

      testAnimationSampleMatrices(animation, idlName,
        [ { time: 0,    expected: [ -1, 0, 0, -1, 200,   0 ] },
          { time: 499,  expected: [ -1, 0, 0, -1, 200,   0 ] },
          { time: 500,  expected: [  1, 1, 0,  0,   0, 100 ] },
          { time: 1000, expected: [  1, 1, 0,  0,   0, 100 ] }]);
    }, `${property}: non-invertible matrices`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: [
            // matrix(0, -1, 1, 0, 250, 0)
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) rotate(90deg)',
            // matrix(-1, -1, 0, 0, 100, 100)
            'translate(100px) matrix( 1, 1, 0,  0, 0, 100) rotate(180deg)',
          ],
        },
        { duration: 1000, fill: 'both' }
      );

      testAnimationSampleMatrices(animation, idlName,
        [ { time: 0,    expected: [  0, -1, 1, 0, 250,   0 ] },
          { time: 499,  expected: [  0, -1, 1, 0, 250,   0 ] },
          { time: 500,  expected: [ -1, -1, 0, 0, 100, 100 ] },
          { time: 1000, expected: [ -1, -1, 0, 0, 100, 100 ] }]);
    }, `${property}: non-invertible matrices in matched transform lists`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: [
            // matrix(-2, 0, 0, -2, 250, 0)
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) scale(2)',
            // matrix(1, 1, 1, 1, 100, 100)
            'translate(100px) matrix( 1, 1, 0,  0, 0, 100) skew(45deg)',
          ],
        },
        { duration: 1000, fill: 'both' }
      );

      testAnimationSampleMatrices(animation, idlName,
        [ { time: 0,    expected: [ -2, 0, 0, -2, 250,   0 ] },
          { time: 499,  expected: [ -2, 0, 0, -2, 250,   0 ] },
          { time: 500,  expected: [  1, 1, 1,  1, 100, 100 ] },
          { time: 1000, expected: [  1, 1, 1,  1, 100, 100 ] }]);
    }, `${property}: non-invertible matrices in mismatched transform lists`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          // perspective(0) is treated as perspective(1px)
          [idlName]: ['perspective(0)', 'perspective(10px)'],
        },
        1000
      );
      testAnimationSampleMatrices(animation, idlName,
        [{ time: 500,  expected: [ 1, 0, 0, 0,
                                   0, 1, 0, 0,
                                   0, 0, 1, -0.55,
                                   0, 0, 0, 1 ] }]);
    }, `${property}: perspective`);

  },

  testAddition: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'translateX(100px)';
      const animation = target.animate({ [idlName]: ['translateX(-200px)',
                                                     'translateX(500px)'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });
      testAnimationSampleMatrices(animation, idlName,
        [ { time: 0,    expected: [ 1, 0, 0, 1, -100, 0 ] },
          { time: 1000, expected: [ 1, 0, 0, 1,  600, 0 ] }]);
    }, `${property}: translate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rotate(45deg)';
      const animation = target.animate({ [idlName]: ['rotate(-90deg)',
                                                     'rotate(90deg)'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ Math.cos(-Math.PI / 4),
                                   Math.sin(-Math.PI / 4),
                                  -Math.sin(-Math.PI / 4),
                                   Math.cos(-Math.PI / 4),
                                   0, 0] },
         { time: 1000, expected: [ Math.cos(Math.PI * 3 / 4),
                                   Math.sin(Math.PI * 3 / 4),
                                  -Math.sin(Math.PI * 3 / 4),
                                   Math.cos(Math.PI * 3 / 4),
                                   0, 0] }]);
    }, `${property}: rotate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'scale(2)';
      const animation = target.animate({ [idlName]: ['scale(-3)', 'scale(5)'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ -6, 0, 0, -6, 0, 0 ] }, // scale(-3) scale(2)
         { time: 1000, expected: [ 10, 0, 0, 10, 0, 0 ] }]); // scale(5) scale(2)
    }, `${property}: scale`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
                              // matrix(1, tan(10deg), tan(10deg), 1)
      target.style[idlName] = 'skew(10deg, 10deg)';
      const animation =              // matrix(1, tan(20deg), tan(-30deg), 1)
        target.animate({ [idlName]: ['skew(-30deg, 20deg)',
                                     // matrix(1, tan(-30deg), tan(20deg), 1)
                                     'skew(20deg, -30deg)'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      // matrix at 0%.
      // [ 1          tan(10deg) ] [ 1          tan(-30deg) ]
      // [ tan(10deg)          1 ] [ tan(20deg)           1 ] =
      //
      // [ 1 + tan(10deg) * tan(20deg) tan(-30deg) + tan(10deg)     ]
      // [     tan(10deg) + tan(20deg) tan(10deg) * tan(-30deg) + 1 ]

      // matrix at 100%.
      // [ 1          tan(10deg) ] [ 1           tan(20deg) ]
      // [ tan(10deg)          1 ] [ tan(-30deg)          1 ] =
      //
      // [ 1 + tan(10deg) * tan(-30deg) tan(20deg) + tan(10deg)     ]
      // [     tan(10deg) + tan(-30deg) tan(10deg) * tan(20deg) + 1 ]

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ 1 + Math.tan(Math.PI/18) * Math.tan(Math.PI/9),
                                   Math.tan(Math.PI/18) + Math.tan(Math.PI/9),
                                   Math.tan(-Math.PI/6) + Math.tan(Math.PI/18),
                                   1 + Math.tan(Math.PI/18) * Math.tan(-Math.PI/6),
                                   0, 0] },
         { time: 1000, expected: [ 1 + Math.tan(Math.PI/18) * Math.tan(-Math.PI/6),
                                   Math.tan(Math.PI/18) + Math.tan(-Math.PI/6),
                                   Math.tan(Math.PI/9) + Math.tan(Math.PI/18),
                                   1 + Math.tan(Math.PI/18) * Math.tan(Math.PI/9),
                                   0, 0] }]);
    }, `${property}: skew`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
                               // matrix(1, 0, 0, 1, 100, 0)
      target.style[idlName] = 'translateX(100px)';
      const animation =              // matrix(0, 1, -1, 0, 0, 0)
        target.animate({ [idlName]: ['rotate(90deg)',
                                     // matrix(-1, 0, 0, -1, 0, 0)
                                     'rotate(180deg)'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [  0, 1, -1,  0, 100, 0 ] },
         { time: 1000, expected: [ -1, 0,  0, -1, 100, 0 ] }]);
    }, `${property}: rotate on translate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
                               // matrix(0, 1, -1, 0, 0, 0)
      target.style[idlName] = 'rotate(90deg)';
      const animation =              // matrix(1, 0, 0, 1, 100, 0)
        target.animate({ [idlName]: ['translateX(100px)',
                                     // matrix(1, 0, 0, 1, 200, 0)
                                     'translateX(200px)'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ 0, 1, -1, 0, 0, 100 ] },
         { time: 1000, expected: [ 0, 1, -1, 0, 0, 200 ] }]);
    }, `${property}: translate on rotate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rotate(45deg) translateX(100px)';
      const animation = target.animate({ [idlName]: ['rotate(-90deg)',
                                                     'rotate(90deg)'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ Math.cos(-Math.PI / 4),
                                   Math.sin(-Math.PI / 4),
                                  -Math.sin(-Math.PI / 4),
                                   Math.cos(-Math.PI / 4),
                                   100 * Math.cos(Math.PI / 4),
                                   100 * Math.sin(Math.PI / 4) ] },
         { time: 1000, expected: [ Math.cos(Math.PI * 3 / 4),
                                   Math.sin(Math.PI * 3 / 4),
                                  -Math.sin(Math.PI * 3 / 4),
                                   Math.cos(Math.PI * 3 / 4),
                                   100 * Math.cos(Math.PI / 4),
                                   100 * Math.sin(Math.PI / 4) ] }]);
    }, `${property}: rotate on rotate and translate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'matrix(0, 1, -1, 0, 0, 0)';
      const animation =               // Same matrices as above.
        target.animate({ [idlName]: [ 'matrix(1, 0, 0, 1, 100, 0)',
                                      'matrix(1, 0, 0, 1, 200, 0)' ] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ 0, 1, -1, 0, 0, 100 ] },
         { time: 1000, expected: [ 0, 1, -1, 0, 0, 200 ] }]);
    }, `${property}: matrix`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rotate3d(1, 1, 0, 45deg)';
      const animation =
        target.animate({ [idlName]: [ 'rotate3d(1, 1, 0, -90deg)',
                                      'rotate3d(1, 1, 0, 90deg)'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: rotate3dToMatrix(1, 1, 0,    -Math.PI / 4) },
         { time: 1000, expected: rotate3dToMatrix(1, 1, 0, 3 * Math.PI / 4) }]);
    }, `${property}: rotate3d`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      // To calculate expected matrices easily, generate input matrices from
      // rotate3d.
      target.style[idlName] = rotate3dToMatrix3d(1, 1, 0, Math.PI / 4);
      const from = rotate3dToMatrix3d(1, 1, 0, -Math.PI / 2);
      const to = rotate3dToMatrix3d(1, 1, 0, Math.PI / 2);
      const animation =
        target.animate({ [idlName]: [ from, to ] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: rotate3dToMatrix(1, 1, 0,    -Math.PI / 4) },
         { time: 1000, expected: rotate3dToMatrix(1, 1, 0, 3 * Math.PI / 4) }]);
    }, `${property}: matrix3d`);

    // Following tests aim for test the addition behavior for non-invertible
    // matrices. Note that the addition for non-invertible matrices should be
    // the same, just like addition for invertible matrices. With these tests,
    // we can assure that addition never behaves as discrete. The non-invertible
    // matrix that we use is the singular matrix, matrix(1, 1, 0, 0, 0, 100).
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'translateX(50px)';
      const animation =
        target.animate({ [idlName]: ['matrix(-1, 0, 0, -1, 200, 0)',
                                     'matrix( 1, 1, 0,  0, 0, 100)'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [ { time: 0,    expected: [ -1, 0, 0, -1, 250,   0 ] },
          { time: 1000, expected: [  1, 1, 0,  0,  50, 100 ] }]);
    }, `${property}: non-invertible matrices`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'translateX(50px)';
      const animation =              // matrix(0, -1, 1, 0, 200, 0)
        target.animate({ [idlName]: ['matrix(-1, 0, 0, -1, 200, 0) rotate(90deg)',
                                     // matrix(-1, -1, 0, 0, 0, 100)
                                     'matrix( 1, 1, 0,  0, 0, 100) rotate(180deg)'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [ { time: 0,    expected: [  0, -1, 1, 0, 250,   0 ] },
          { time: 1000, expected: [ -1, -1, 0, 0,  50, 100 ] }]);
    }, `${property}: non-invertible matrices in matched transform lists`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'translateX(50px)';
      const animation =                // matrix(-2, 0, 0, -2, 200, 0)
        target.animate({ [idlName]: ['matrix(-1, 0, 0, -1, 200, 0) scale(2)',
                                     // matrix(1, 1, 1, 1, 0, 100)
                                     'matrix( 1, 1, 0,  0, 0, 100) skew(45deg)'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleMatrices(animation, idlName,
        [ { time: 0,    expected: [ -2, 0, 0, -2, 250,   0 ] },
          { time: 1000, expected: [  1, 1, 1,  1,  50, 100 ] }]);
    }, `${property}: non-invertible matrices in mismatched transform lists`);
  },

  testAccumulation: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'translateX(100px)';
      const animation = target.animate({ [idlName]: ['translateX(-200px)',
                                                   'translateX(500px)'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });
      testAnimationSampleMatrices(animation, idlName,
        [ { time: 0,    expected: [ 1, 0, 0, 1, -100, 0 ] },
          { time: 1000, expected: [ 1, 0, 0, 1,  600, 0 ] }]);
    }, `${property}: translate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rotate(45deg)';
      const animation = target.animate({ [idlName]: ['rotate(-90deg)',
                                                   'rotate(90deg)'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ Math.cos(-Math.PI / 4),
                                   Math.sin(-Math.PI / 4),
                                  -Math.sin(-Math.PI / 4),
                                   Math.cos(-Math.PI / 4),
                                   0, 0] },
         { time: 1000, expected: [ Math.cos(Math.PI * 3 / 4),
                                   Math.sin(Math.PI * 3 / 4),
                                  -Math.sin(Math.PI * 3 / 4),
                                   Math.cos(Math.PI * 3 / 4),
                                   0, 0] }]);
    }, `${property}: rotate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'scale(2)';
      const animation = target.animate({ [idlName]: ['scale(-3)', 'scale(5)'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
                                  // scale((2 - 1) + (-3 - 1) + 1)
        [{ time: 0,    expected: [ -2, 0, 0, -2, 0, 0 ] },
                                  // scale((2 - 1) + (5 - 1) + 1)
         { time: 1000, expected: [  6, 0, 0,  6, 0, 0 ] }]);
    }, `${property}: scale`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
                              // matrix(1, tan(10deg), tan(10deg), 1)
      target.style[idlName] = 'skew(10deg, 10deg)';
      const animation =                // matrix(1, tan(20deg), tan(-30deg), 1)
        target.animate({ [idlName]: ['skew(-30deg, 20deg)',
                                     // matrix(1, tan(-30deg), tan(20deg), 1)
                                     'skew(20deg, -30deg)'] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ 1, Math.tan(Math.PI/6),
                                   Math.tan(-Math.PI/9), 1,
                                   0, 0] },
         { time: 1000, expected: [ 1, Math.tan(-Math.PI/9),
                                   Math.tan(Math.PI/6), 1,
                                   0, 0] }]);
    }, `${property}: skew`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
                               // matrix(1, 0, 0, 1, 100, 0)
      target.style[idlName] = 'translateX(100px)';
      const animation =                // matrix(0, 1, -1, 0, 0, 0)
        target.animate({ [idlName]: ['rotate(90deg)',
                                     // matrix(-1, 0, 0, -1, 0, 0)
                                     'rotate(180deg)'] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [  0, 1, -1,  0, 100, 0 ] },
         { time: 1000, expected: [ -1, 0,  0, -1, 100, 0 ] }]);
    }, `${property}: rotate on translate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
                               // matrix(0, 1, -1, 0, 0, 0)
      target.style[idlName] = 'rotate(90deg)';
      const animation =                // matrix(1, 0, 0, 1, 100, 0)
        target.animate({ [idlName]: ['translateX(100px)',
                                     // matrix(1, 0, 0, 1, 200, 0)
                                     'translateX(200px)'] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ 0, 1, -1, 0, 100, 0 ] },
         { time: 1000, expected: [ 0, 1, -1, 0, 200, 0 ] }]);
    }, `${property}: translate on rotate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rotate(45deg)';
      const animation =
        target.animate({ [idlName]: ['rotate(45deg) translateX(0px)',
                                     'rotate(45deg) translateX(100px)'] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ Math.cos(Math.PI / 2),
                                   Math.sin(Math.PI / 2),
                                  -Math.sin(Math.PI / 2),
                                   Math.cos(Math.PI / 2),
                                   0 * Math.cos(Math.PI / 2),
                                   0 * Math.sin(Math.PI / 2) ] },
         { time: 1000, expected: [ Math.cos(Math.PI / 2),
                                   Math.sin(Math.PI / 2),
                                  -Math.sin(Math.PI / 2),
                                   Math.cos(Math.PI / 2),
                                   100 * Math.cos(Math.PI / 2),
                                   100 * Math.sin(Math.PI / 2) ] }]);
    }, `${property}: rotate and translate on rotate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rotate(45deg) translateX(100px)';
      const animation =
        target.animate({ [idlName]: ['rotate(45deg)', 'rotate(45deg)'] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ Math.cos(Math.PI / 2),
                                   Math.sin(Math.PI / 2),
                                  -Math.sin(Math.PI / 2),
                                   Math.cos(Math.PI / 2),
                                   100 * Math.cos(Math.PI / 2),
                                   100 * Math.sin(Math.PI / 2) ] },
         { time: 1000, expected: [ Math.cos(Math.PI / 2),
                                   Math.sin(Math.PI / 2),
                                  -Math.sin(Math.PI / 2),
                                   Math.cos(Math.PI / 2),
                                   100 * Math.cos(Math.PI / 2),
                                   100 * Math.sin(Math.PI / 2) ] }]);
    }, `${property}: rotate on rotate and translate`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'matrix(0, 1, -1, 0, 0, 0)';
      const animation =                 // Same matrices as above.
        target.animate({ [idlName]: [ 'matrix(1, 0, 0, 1, 100, 0)',
                                      'matrix(1, 0, 0, 1, 200, 0)' ] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: [ 0, 1, -1, 0, 100, 0 ] },
         { time: 1000, expected: [ 0, 1, -1, 0, 200, 0 ] }]);
    }, `${property}: matrix`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rotate3d(1, 1, 0, 45deg)';
      const animation =
        target.animate({ [idlName]: [ 'rotate3d(1, 1, 0, -90deg)',
                                      'rotate3d(1, 1, 0, 90deg)'] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: rotate3dToMatrix(1, 1, 0,    -Math.PI / 4) },
         { time: 1000, expected: rotate3dToMatrix(1, 1, 0, 3 * Math.PI / 4) }]);
    }, `${property}: rotate3d`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      // To calculate expected matrices easily, generate input matrices from
      // rotate3d.
      target.style[idlName] = rotate3dToMatrix3d(1, 1, 0, Math.PI / 4);
      const from = rotate3dToMatrix3d(1, 1, 0, -Math.PI / 2);
      const to = rotate3dToMatrix3d(1, 1, 0, Math.PI / 2);
      const animation =
        target.animate({ [idlName]: [ from, to ] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: rotate3dToMatrix(1, 1, 0,    -Math.PI / 4) },
         { time: 1000, expected: rotate3dToMatrix(1, 1, 0, 3 * Math.PI / 4) }]);
    }, `${property}: matrix3d`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const matrixArray = [ 1, 0, 0, 0,
                          0, 1, 0, 0,
                          0, 0, 1, 0,
                          0, 0, 1, 1 ];

      target.style[idlName] = createMatrixFromArray(matrixArray);
      const animation =
        target.animate({ [idlName]: [ 'none', 'none' ] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleMatrices(animation, idlName,
        [{ time: 0,    expected: matrixArray },
         { time: 1000, expected: matrixArray }]);
    }, `${property}: none`);

    // Following tests aim for test the fallback discrete accumulation behavior
    // for non-invertible matrices. The non-invertible matrix that we use is the
    // singular matrix, matrix(1, 1, 0, 0, 0, 100).
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.animate({ [idlName]: ['matrix(-1, 0, 0, -1, 200, 0)',
                                   'matrix(-1, 0, 0, -1, 200, 0)'] }, 1000);
      const animation = target.animate(
        {
          [idlName]: [
            'matrix( 1, 1, 0, 0, 0, 100)',
            'matrix( 1, 1, 0, 0, 0, 100)',
          ],
        },
        { duration: 1000, composite: 'accumulate' }
      );
      testAnimationSampleMatrices(animation, idlName, [
        { time: 0, expected: [1, 1, 0, 0, 0, 100] },
      ]);
    }, `${property}: non-invertible matrices (non-invertible onto invertible)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.animate({ [idlName]: ['matrix( 1, 1, 0, 0, 0, 100)',
                                   'matrix( 1, 1, 0, 0, 0, 100)'] }, 1000);
      const animation = target.animate(
        {
          [idlName]: [
            'matrix(-1, 0, 0, -1, 200, 0)',
            'matrix(-1, 0, 0, -1, 200, 0)',
          ],
        },
        { duration: 1000, composite: 'accumulate' }
      );
      testAnimationSampleMatrices(animation, idlName, [
        { time: 0, expected: [-1, 0, 0, -1, 200, 0] },
      ]);
    }, `${property}: non-invertible matrices (invertible onto non-invertible)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      // matrix(0, -1, 1, 0, 250, 0)
      target.animate(
        {
          [idlName]: [
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) rotate(90deg)',
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) rotate(90deg)',
          ],
        },
        1000
      );
      // matrix(-1, -1, 0, 0, 100, 100)
      const animation = target.animate(
        {
          [idlName]: [
            'translate(100px) matrix( 1, 1, 0, 0, 0, 100) rotate(180deg)',
            'translate(100px) matrix( 1, 1, 0, 0, 0, 100) rotate(180deg)',
          ],
        },
        { duration: 1000, composite: 'accumulate' }
      );
      testAnimationSampleMatrices(animation, idlName, [
        { time: 0, expected: [-1, -1, 0, 0, 100, 100] },
      ]);
    }, `${property}: non-invertible matrices in matched transform lists (non-invertible onto invertible)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      // matrix(-1, -1, 0, 0, 100, 100)
      target.animate(
        {
          [idlName]: [
            'translate(100px) matrix(1, 1, 0, 0, 0, 100) rotate(180deg)',
            'translate(100px) matrix(1, 1, 0, 0, 0, 100) rotate(180deg)',
          ],
        },
        1000
      );
      // matrix(0, -1, 1, 0, 250, 0)
      const animation = target.animate(
        {
          [idlName]: [
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) rotate(90deg)',
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) rotate(90deg)',
          ],
        },
        { duration: 1000, composite: 'accumulate' }
      );
      testAnimationSampleMatrices(animation, idlName, [
        { time: 0, expected: [0, -1, 1, 0, 250, 0] },
      ]);
    }, `${property}: non-invertible matrices in matched transform lists (invertible onto non-invertible)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      // matrix(-2, 0, 0, -2, 250, 0)
      target.animate(
        {
          [idlName]: [
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) scale(2)',
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) scale(2)',
          ],
        },
        1000
      );
      // matrix(1, 1, 1, 1, 100, 100)
      const animation = target.animate(
        {
          [idlName]: [
            'translate(100px) matrix(1, 1, 0, 0, 0, 100) skew(45deg)',
            'translate(100px) matrix(1, 1, 0, 0, 0, 100) skew(45deg)',
          ],
        },
        { duration: 1000, composite: 'accumulate' }
      );
      testAnimationSampleMatrices(animation, idlName, [
        { time: 0, expected: [1, 1, 1, 1, 100, 100] },
      ]);
    }, `${property}: non-invertible matrices in mismatched transform lists`
       + ' (non-invertible onto invertible)');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      // matrix(1, 1, 1, 1, 100, 100)
      target.animate(
        {
          [idlName]: [
            'translate(100px) matrix(1, 1, 0, 0, 0, 100) skew(45deg)',
            'translate(100px) matrix(1, 1, 0, 0, 0, 100) skew(45deg)',
          ],
        },
        1000
      );
      // matrix(-2, 0, 0, -2, 250, 0)
      const animation = target.animate(
        {
          [idlName]: [
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) scale(2)',
            'translate(50px)  matrix(-1, 0, 0, -1, 200, 0) scale(2)',
          ],
        },
        { duration: 1000, composite: 'accumulate' }
      );
      testAnimationSampleMatrices(animation, idlName, [
        { time: 0, expected: [-2, 0, 0, -2, 250, 0] },
      ]);
    }, `${property}: non-invertible matrices in mismatched transform lists`
       + ' (invertible onto non-invertible)');
  },
};

const rotateListType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['45deg', '135deg'],
        },
        1000
      );

      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: '90deg' }]);
    }, `${property} without rotation axes`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ '0 1 0 0deg',
                                      '0 1 0 90deg'] },
                       1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500, expected: 'y 45deg' }]);
    }, `${property} with rotation axes`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);

      const animation =
        target.animate({ [idlName]: [ '0 1 0 0deg',
                                      '0 1 0 720deg'] },
                       1000);

      testAnimationSamples(animation, idlName,
        [{ time: 250, expected: 'y 180deg' }]);
    }, `${property} with rotation axes and range over 360 degrees`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ '0 1 0 0deg',
                                      '0 1 1 90deg'] },
                       1000);

      testAnimationSampleRotate3d(animation, idlName,
        [{ time: 500, expected: '0 0.707107 0.707107 45deg' }]);
    }, `${property} with different rotation axes`);
  },
  testAddition: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '45deg';
      const animation = target.animate({ [idlName]: ['-90deg', '90deg'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: '-45deg' },
         { time: 1000, expected: '135deg' }]);
    }, `${property} without rotation axes`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      // Rotation specified in transform property should not affect the computed
      // value of |property|.
      target.style.transform = 'rotate(20deg)';
      target.style[idlName] = 'y -45deg';
      const animation =
        target.animate({ [idlName]: ['0 1 0 90deg',
                                     '0 1 0 180deg'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: 'y 45deg' },
         { time: 1000, expected: 'y 135deg' }]);
    }, `${property} with underlying transform`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ '0 1 0 0deg',
                                      '1 1 1 270deg'] },
                       { duration: 1000, fill: 'both', composite: 'add' });

      testAnimationSampleRotate3d(animation, idlName,
        [{ time: 500, expected: '0.57735 0.57735 0.57735 135deg' }]);
    }, `${property} with different rotation axes`);
  },
  testAccumulation: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '45deg';
      const animation = target.animate({ [idlName]: ['-90deg', '90deg'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: '-45deg' },
         { time: 1000, expected: '135deg' }]);
    }, `${property} without rotation axes`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style.transform = 'translateX(100px)';
      target.style[idlName] = '1 0 0 -45deg';
      const animation =
        target.animate({ [idlName]: ['1 0 0 90deg',
                                     '1 0 0 180deg'] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: 'x 45deg' },
         { time: 1000, expected: 'x 135deg' }]);
    }, `${property} with underlying transform`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ '0 1 0 0deg',
                                      '1 0 1 180deg'] },
                       { duration: 1000, fill: 'both', composite: 'accumulate' });

      testAnimationSampleRotate3d(animation, idlName,
        [{ time: 500, expected: '0.707107 0 0.707107 90deg' }]);
    }, `${property} with different rotation axes`);
  },
};

const translateListType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['200px', '400px'],
        },
        1000
      );
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: '300px' }]);
    }, `${property} with two unspecified values`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['200px -200px', '400px 400px'],
        },
        1000
      );
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: '300px 100px' }]);
    }, `${property} with one unspecified value`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['200px -200px 600px', '400px 400px -200px'],
        },
        1000
      );
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: '300px 100px 200px' }]);
    }, `${property} with all three values specified`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        {
          [idlName]: ['0% -101px 600px', '400px 50% -200px'],
        },
        1000
      );
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'calc(0% + 200px) calc(25% - 50.5px) 200px' }]);
    }, `${property} with combination of percentages and lengths`);
  },
  testAddition: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '100px';
      const animation = target.animate({ [idlName]: ['-200px', '500px'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: '-100px' },
          { time: 1000, expected: '600px' }]);

    }, `${property}`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.transform = 'translateY(100px)';
      target.style[idlName] = '100px';
      const animation = target.animate({ [idlName]: ['-200px', '500px'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: '-100px' },
          { time: 1000, expected: '600px' }]);

    }, `${property} with underlying transform`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '50%';
      const animation = target.animate({ [idlName]: ['-200px', '500px'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: 'calc(50% - 200px)' },
          { time: 1000, expected: 'calc(50% + 500px)' }]);

    }, `${property} with underlying percentage value`);
  },
  testAccumulation: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '100px';
      const animation = target.animate({ [idlName]: ['-200px', '500px'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: '-100px' },
          { time: 1000, expected: '600px' }]);
    }, `${property}`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.transform = 'translateY(100px)';
      target.style[idlName] = '100px';
      const animation = target.animate({ [idlName]: ['-200px', '500px'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: '-100px' },
          { time: 1000, expected: '600px' }]);
    }, `${property} with transform`);
  },
};

const scaleListType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['3', '5'] },
                                       1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: '4' }]);
    }, `${property} with two unspecified values`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['3 3', '5 5'] },
                                       1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: '4' }]);
    }, `${property} with one unspecified value`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['3 3 3', '5 5 5'] },
                                       1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: '4 4 4' }]);
    }, `${property}`);
  },
  testAddition: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2';
      const animation = target.animate({ [idlName]: ['-3', '5'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: '-6' },
         { time: 1000, expected: '10' }]);
    }, `${property} with two unspecified values`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2 2';
      const animation = target.animate({ [idlName]: ['-3 -3', '5 5'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: '-6' },
         { time: 1000, expected: '10' }]);
    }, `${property} with one unspecified value`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2 2 2';
      const animation = target.animate({ [idlName]: ['-1 -2 -3', '4 5 6'] },
                                       { duration: 1000, fill: 'both',
                                         composite: 'add' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: '-2 -4 -6' },
         { time: 1000, expected: '8 10 12' }]);
    }, `${property}`);
  },
  testAccumulation: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2';
      const animation = target.animate({ [idlName]: ['-3', '5'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: '-2' },
         { time: 1000, expected: '6' }]);
    }, `${property} with two unspecified values`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2 2';
      const animation = target.animate({ [idlName]: ['-3 -3', '5 5'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: '-2' },
         { time: 1000, expected: '6' }]);
    }, `${property} with one unspecified value`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '2 2 2';
      const animation = target.animate({ [idlName]: ['-1 -2 -3', '4 5 6'] },
                                     { duration: 1000, fill: 'both',
                                       composite: 'accumulate' });

      testAnimationSamples(animation, idlName,
        [{ time: 0,    expected: '0 -1 -2' },
         { time: 1000, expected: '5 6 7' }]);
    }, `${property}`);
  },
};

const filterListType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]:
                                         ['blur(10px)', 'blur(50px)'] },
                                        1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,    expected: 'blur(30px)' }]);
    }, `${property}: blur function`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['hue-rotate(0deg)',
                                                     'hue-rotate(100deg)'] },
                                       1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,    expected: 'hue-rotate(50deg)' }]);
    }, `${property}: hue-rotate function with same unit(deg)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['hue-rotate(10deg)',
                                                     'hue-rotate(100rad)'] },
                                       1000);

      // 10deg = 0.1745rad.
      testAnimationSamples(animation, idlName,
        [{ time: 500,    expected: 'hue-rotate(2869.79deg)' }]);
    }, `${property}: hue-rotate function with different unit(deg -> rad)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]:
          ['drop-shadow(10px 10px 10px rgba(255, 0, 0, 0.4))',
           'drop-shadow(50px 50px 50px rgba(0, 0, 255, 0.8))'] },
        1000);

      testAnimationSamples(
        animation, idlName,
        [{ time: 500,
            expected: 'drop-shadow(rgba(85, 0, 170, 0.6) 30px 30px 30px)' }]);
    }, `${property}: drop-shadow function`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]:
          ['brightness(0.1) contrast(0.1) grayscale(0.1) invert(0.1) ' +
           'opacity(0.1) saturate(0.1) sepia(0.1)',
           'brightness(0.5) contrast(0.5) grayscale(0.5) invert(0.5) ' +
           'opacity(0.5) saturate(0.5) sepia(0.5)'] },
        1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,
           expected: 'brightness(0.3) contrast(0.3) grayscale(0.3) ' +
           'invert(0.3) opacity(0.3) saturate(0.3) sepia(0.3)' }]);
    }, `${property}: percentage or numeric-specifiable functions`
       + ' (number value)');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]:
          ['brightness(10%) contrast(10%) grayscale(10%) invert(10%) ' +
           'opacity(10%) saturate(10%) sepia(10%)',
           'brightness(50%) contrast(50%) grayscale(50%) invert(50%) ' +
           'opacity(50%) saturate(50%) sepia(50%)'] },
        1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,
           expected: 'brightness(0.3) contrast(0.3) grayscale(0.3) ' +
           'invert(0.3) opacity(0.3) saturate(0.3) sepia(0.3)' }]);
    }, `${property}: percentage or numeric-specifiable functions`
       + ' (percentage value)');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]:
          // To make missing filter-function-lists, specified the grayscale.
          ['grayscale(0)',
           'grayscale(1) brightness(0) contrast(0) opacity(0) saturate(0)' ]},
        1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,
           expected: 'grayscale(0.5) brightness(0.5) contrast(0.5) ' +
                     'opacity(0.5) saturate(0.5)' }]);
    }, `${property}: interpolate different length of filter-function-list`
       + ' with function which lacuna value is 1');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]:
          // To make missing filter-function-lists, specified the opacity.
          ['opacity(1)',
           'opacity(0) grayscale(1) invert(1) sepia(1) blur(10px)'] },
        1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,
           expected:
           'opacity(0.5) grayscale(0.5) invert(0.5) sepia(0.5) blur(5px)' }]);
    }, `${property}: interpolate different length of filter-function-list`
       + ' with function which lacuna value is 0');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]:
          ['blur(0px)',
           'blur(10px) drop-shadow(10px 10px 10px rgba(0, 0, 255, 0.8))'] },
        1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500,
           // Per the spec: The initial value for interpolation is all length values
           // set to 0 and the used color set to transparent.
           expected: 'blur(5px) drop-shadow(rgba(0, 0, 255, 0.4) 5px 5px 5px)' }]);
    }, `${property}: interpolate different length of filter-function-list`
       + ' with drop-shadow function');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['none', 'blur(10px)'] },
                                     1000);

      testAnimationSamples(animation, idlName,
        [{ time: 500, expected: 'blur(5px)' }]);
    }, `${property}: interpolate from none`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate(
        { [idlName]:
          ['blur(0px) url(\"#f1\")',
           'blur(10px) url(\"#f2\")']},
        1000);
      testAnimationSamples(animation, idlName,
        [{ time: 499, expected: 'blur(0px) url(\"#f1\")' },
         { time: 500, expected: 'blur(10px) url(\"#f2\")' }]);
    }, `${property}: url function (interpoalte as discrete)`);
  },

  testAddition: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'blur(10px)';
      const animation = target.animate({ [idlName]: ['blur(20px)',
                                                     'blur(50px)'] },
                                       { duration: 1000, composite: 'add' });
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: 'blur(10px) blur(20px)' }]);
    }, `${property}: blur on blur`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'blur(10px)';
      const animation = target.animate({ [idlName]: ['brightness(80%)',
                                                     'brightness(40%)'] },
                                       { duration: 1000, composite: 'add' });
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: 'blur(10px) brightness(0.8)' }]);
    }, `${property}: different filter functions`);
  },

  testAccumulation: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'blur(10px) brightness(0.3)';
      const animation = target.animate(
        {
          [idlName]: [
            'blur(20px) brightness(0.1)',
            'blur(20px) brightness(0.1)',
          ],
        },
        { duration: 1000, composite: 'accumulate' }
      );
      // brightness(0.1) onto brightness(0.3) means
      // brightness((0.1 - 1.0) + (0.3 - 1.0) + 1.0). The result of this formula
      // is brightness(-0.6) that means brightness(0.0).
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: 'blur(30px) brightness(0)' }]);
    }, `${property}: same ordered filter functions`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'blur(10px) brightness(1.3)';
      const animation = target.animate(
        {
          [idlName]: [
            'brightness(1.2) blur(20px)',
            'brightness(1.2) blur(20px)',
          ],
        },
        { duration: 1000, composite: 'accumulate' }
      );
      // Mismatched ordered functions can't be accumulated.
      testAnimationSamples(animation, idlName,
        [ { time: 0,    expected: 'brightness(1.2) blur(20px)' }]);
    }, `${property}: mismatched ordered filter functions`);
  },
};

const textShadowListType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'none',
                                      'rgb(100, 100, 100) 10px 10px 10px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                                  // Premultiplied
        [{ time: 500,  expected: 'rgba(100, 100, 100, 0.5) 5px 5px 5px' }]);
    }, `${property}: from none to other`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(100, 100, 100) 10px 10px 10px',
                                      'none' ] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                                  // Premultiplied
        [{ time: 500,  expected: 'rgba(100, 100, 100, 0.5) 5px 5px 5px' }]);
    }, `${property}: from other to none`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(0, 0, 0) 0px 0px 0px',
                                      'rgb(100, 100, 100) 10px 10px 10px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(50, 50, 50) 5px 5px 5px' }]);
    }, `${property}: single shadow`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(0, 0, 0) 0px 0px 0px, '
                                    + 'rgb(200, 200, 200) 20px 20px 20px',
                                      'rgb(100, 100, 100) 10px 10px 10px, '
                                    + 'rgb(100, 100, 100) 10px 10px 10px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(50, 50, 50) 5px 5px 5px, '
                               + 'rgb(150, 150, 150) 15px 15px 15px' }]);
    }, `${property}: shadow list`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(200, 200, 200) 20px 20px 20px',
                                      'rgb(100, 100, 100) 10px 10px 10px, '
                                    + 'rgb(100, 100, 100) 10px 10px 10px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(150, 150, 150) 15px 15px 15px, '
                               + 'rgba(100, 100, 100, 0.5) 5px 5px 5px' }]);
    }, `${property}: mismatched list length (from longer to shorter)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(100, 100, 100) 10px 10px 10px, '
                                    + 'rgb(100, 100, 100) 10px 10px 10px',
                                      'rgb(200, 200, 200) 20px 20px 20px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(150, 150, 150) 15px 15px 15px, '
                               + 'rgba(100, 100, 100, 0.5) 5px 5px 5px' }]);
    }, `${property}: mismatched list length (from shorter to longer)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style.color = 'rgb(0, 255, 0)';
      const animation =
        target.animate({ [idlName]: [ 'currentcolor 0px 0px 0px',
                                      'currentcolor 10px 10px 10px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(0, 255, 0) 5px 5px 5px' }]);
    }, `${property}: with currentcolor`);
  },

  testAddition: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(0, 0, 0) 0px 0px 0px';
      const animation =
        target.animate({ [idlName]: [ 'rgb(120, 120, 120) 10px 10px 10px',
                                      'rgb(120, 120, 120) 10px 10px 10px'] },
                       { duration: 1000, composite: 'add' });
      testAnimationSamples(animation, idlName,
        [ { time: 0, expected: 'rgb(0, 0, 0) 0px 0px 0px, ' +
                               'rgb(120, 120, 120) 10px 10px 10px' }]);
    }, `${property}: shadow`);
  },

  testAccumulation: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(120, 120, 120) 10px 10px 10px';
      const animation =
        target.animate({ [idlName]: [ 'rgb(120, 120, 120) 10px 10px 10px',
                                      'rgb(120, 120, 120) 10px 10px 10px'] },
                       { duration: 1000, composite: 'accumulate' });
      testAnimationSamples(animation, idlName,
        [ { time: 0, expected: 'rgb(240, 240, 240) 20px 20px 20px' }]);
    }, `${property}: shadow`);
  },
};


const boxShadowListType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'none',
                                      'rgb(100, 100, 100) 10px 10px 10px 0px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                                  // Premultiplied
        [{ time: 500,  expected: 'rgba(100, 100, 100, 0.5) 5px 5px 5px 0px' }]);
    }, `${property}: from none to other`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(100, 100, 100) 10px 10px 10px 0px',
                                      'none' ] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                                  // Premultiplied
        [{ time: 500,  expected: 'rgba(100, 100, 100, 0.5) 5px 5px 5px 0px' }]);
    }, `${property}: from other to none`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(0, 0, 0) 0px 0px 0px 0px',
                                      'rgb(100, 100, 100) 10px 10px 10px 0px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(50, 50, 50) 5px 5px 5px 0px' }]);
    }, `${property}: single shadow`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(0, 0, 0) 0px 0px 0px 0px, '
                                    + 'rgb(200, 200, 200) 20px 20px 20px 20px',
                                      'rgb(100, 100, 100) 10px 10px 10px 0px, '
                                    + 'rgb(100, 100, 100) 10px 10px 10px 0px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(50, 50, 50) 5px 5px 5px 0px, '
                               + 'rgb(150, 150, 150) 15px 15px 15px 10px' }]);
    }, `${property}: shadow list`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(200, 200, 200) 20px 20px 20px 20px',
                                      'rgb(100, 100, 100) 10px 10px 10px 0px, '
                                    + 'rgb(100, 100, 100) 10px 10px 10px 0px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(150, 150, 150) 15px 15px 15px 10px, '
                               + 'rgba(100, 100, 100, 0.5) 5px 5px 5px 0px' }]);
    }, `${property}: mismatched list length (from shorter to longer)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: [ 'rgb(100, 100, 100) 10px 10px 10px 0px, '
                                    + 'rgb(100, 100, 100) 10px 10px 10px 0px',
                                      'rgb(200, 200, 200) 20px 20px 20px 20px']},
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(150, 150, 150) 15px 15px 15px 10px, '
                               + 'rgba(100, 100, 100, 0.5) 5px 5px 5px 0px' }]);
    }, `${property}: mismatched list length (from longer to shorter)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style.color = 'rgb(0, 255, 0)';
      const animation =
        target.animate({ [idlName]: [ 'currentcolor 0px 0px 0px 0px',
                                      'currentcolor 10px 10px 10px 10px'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
        [{ time: 500,  expected: 'rgb(0, 255, 0) 5px 5px 5px 5px' }]);
    }, `${property}: with currentcolor`);
  },

  testAddition: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(0, 0, 0) 0px 0px 0px 0px';
      const animation =
        target.animate({ [idlName]: [ 'rgb(120, 120, 120) 10px 10px 10px 0px',
                                      'rgb(120, 120, 120) 10px 10px 10px 0px'] },
                       { duration: 1000, composite: 'add' });
      testAnimationSamples(animation, idlName,
        [ { time: 0, expected: 'rgb(0, 0, 0) 0px 0px 0px 0px, ' +
                               'rgb(120, 120, 120) 10px 10px 10px 0px' }]);
    }, `${property}: shadow`);
  },

  testAccumulation: function(property, setup) {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rgb(120, 120, 120) 10px 10px 10px 10px';
      const animation =
        target.animate({ [idlName]: [ 'rgb(120, 120, 120) 10px 10px 10px 10px',
                                      'rgb(120, 120, 120) 10px 10px 10px 10px'] },
                       { duration: 1000, composite: 'accumulate' });
      testAnimationSamples(animation, idlName,
        [ { time: 0, expected: 'rgb(240, 240, 240) 20px 20px 20px 20px' }]);
    }, `${property}: shadow`);
  },
};

const positionType = {
  testInterpolation: (property, setup) => {
    lengthPairType.testInterpolation(property, setup, false);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]: ['10% 10%', '50% 50%'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(
        animation, idlName,
        [{ time: 500,  expected: calcFromPercentage(idlName, '30% 30%') }]);
    }, `${property} supports animating as a position of percent`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    lengthPairType.testAdditionOrAccumulation(property, setup, composite, false);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '60% 60%';
      const animation = target.animate({ [idlName]: ['70% 70%', '100% 100%'] },
                                       { duration: 1000, composite });
      testAnimationSamples(
        animation, idlName,
        [{ time: 0, expected: calcFromPercentage(idlName, '130% 130%') }]);
    }, `${property}: position of percentage`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
};

const rectType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]:
                                           ['rect(10px, 10px, 10px, 10px)',
                                            'rect(50px, 50px, 50px, 50px)'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(
          animation, idlName,
          [{ time: 500,  expected: 'rect(30px, 30px, 30px, 30px)' }]);
    }, `${property} supports animating as a rect`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = 'rect(100px, 100px, 100px, 100px)';
      const animation = target.animate({ [idlName]:
                                           ['rect(10px, 10px, 10px, 10px)',
                                            'rect(10px, 10px, 10px, 10px)'] },
                                       { duration: 1000, composite });
      testAnimationSamples(
        animation, idlName,
        [{ time: 0, expected: 'rect(110px, 110px, 110px, 110px)' }]);
    }, `${property}: rect`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
}

// stroke-dasharray: none | [ <length> | <percentage> | <number> ]*
const dasharrayType = {
  testInterpolation: (property, setup) => {
    percentageType.testInterpolation(property, setup);
    positiveNumberType.testInterpolation(property, setup, 'px');

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]:
                                           ['8, 16, 4',
                                            '4, 8, 12, 16'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(
          animation, idlName,
          [{ time: 500,  expected: '6px, 12px, 8px, 12px, 10px, 6px, 10px, 16px, 4px, 8px, 14px, 10px' }]);
    }, `${property} supports animating as a dasharray (mismatched length)`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation = target.animate({ [idlName]:
                                           ['2, 50%, 6, 10',
                                            '6, 30%, 2, 2'] },
                                       { duration: 1000, fill: 'both' });
      testAnimationSamples(
          animation, idlName,
          [{ time: 500,  expected: '4px, 40%, 4px, 6px' }]);
    }, `${property} supports animating as a dasharray (mixed lengths and percentages)`);

  },

  // Note that stroke-dasharray is neither additive nor cumulative, so we should
  // write this additive test case that animating value replaces underlying
  // values.
  // See https://www.w3.org/TR/SVG2/painting.html#StrokeDashing.
  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '6, 30%, 2px';
      const animation = target.animate({ [idlName]:
                                           ['1, 2, 3, 4, 5',
                                            '6, 7, 8, 9, 10'] },
                                       { duration: 1000, composite });
      testAnimationSamples(
          animation, idlName,
          [{ time: 0, expected: '1px, 2px, 3px, 4px, 5px' }]);
    }, `${property}: dasharray`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
}

const fontVariationSettingsType = {
  testInterpolation: (property, setup) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['"wght" 1.1', '"wght" 1.5'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamples(animation, idlName,
                           [{ time: 0,  expected: '"wght" 1.1' },
                            { time: 250,  expected: '"wght" 1.2' },
                            { time: 750,  expected: '"wght" 1.4' } ]);
    }, `${property} supports animation as float`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['"wdth" 1, "wght" 1.1',
                                     '"wght" 1.5, "wdth" 5'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamplesWithAnyOrder(
        animation, idlName,
        [{ time: 0, expected: '"wdth" 1, "wght" 1.1' },
         { time: 250, expected: '"wdth" 2, "wght" 1.2' },
         { time: 750, expected: '"wdth" 4, "wght" 1.4' } ]);
    }, `${property} supports animation as float with multiple tags`);

    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      const animation =
        target.animate({ [idlName]: ['"wdth" 1, "wght" 1.1',
                                     '"wght" 10, "wdth" 5, "wght" 1.5'] },
                       { duration: 1000, fill: 'both' });
      testAnimationSamplesWithAnyOrder(
        animation, idlName,
        [{ time: 250, expected: '"wdth" 2, "wght" 1.2' },
         { time: 750, expected: '"wdth" 4, "wght" 1.4' } ]);
    }, `${property} supports animation as float with multiple duplicate tags`);
  },

  testAdditionOrAccumulation: (property, setup, composite) => {
    test(t => {
      const idlName = propertyToIDL(property);
      const target = createTestElement(t, setup);
      target.style[idlName] = '"wght" 1';
      const animation =
        target.animate({ [idlName]: ['"wght" 1.1', '"wght" 1.5'] },
                       { duration: 1000, composite });
      testAnimationSamples(animation, idlName,
                           [{ time: 250,  expected: '"wght" 2.2' },
                            { time: 750,  expected: '"wght" 2.4' } ]);
    }, `${property} with composite type ${composite}`);
  },

  testAddition: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'add');
  },

  testAccumulation: function(property, setup) {
    this.testAdditionOrAccumulation(property, setup, 'accumulate');
  },
}

const types = {
  color: colorType,
  colorPair: colorPairType,
  discrete: discreteType,
  filterList: filterListType,
  integer: integerType,
  positiveInteger: positiveIntegerType,
  length: lengthType,
  percentage: percentageType,
  lengthPercentageOrCalc: lengthPercentageOrCalcType,
  lengthPair: lengthPairType,
  positiveNumber: positiveNumberType,
  opacity: opacityType,
  transformList: transformListType,
  rotateList: rotateListType,
  translateList: translateListType,
  scaleList: scaleListType,
  visibility: visibilityType,
  boxShadowList: boxShadowListType,
  textShadowList: textShadowListType,
  rect: rectType,
  position: positionType,
  dasharray: dasharrayType,
  fontVariationSettings: fontVariationSettingsType,
};
