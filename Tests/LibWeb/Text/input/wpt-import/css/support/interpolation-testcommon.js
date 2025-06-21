'use strict';
(function() {
  var interpolationTests = [];
  var compositionTests = [];
  var cssAnimationsData = {
    sharedStyle: null,
    nextID: 0,
  };
  var expectNoInterpolation = {};
  var expectNotAnimatable = {};
  var neutralKeyframe = {};
  function isNeutralKeyframe(keyframe) {
    return keyframe === neutralKeyframe;
  }

  // For the CSS interpolation methods set the delay to be negative half the
  // duration, so we are immediately at the halfway point of the animation.
  // We then use an easing function that maps halfway to whatever progress
  // we actually want.

  var cssAnimationsInterpolation = {
    name: 'CSS Animations',
    isSupported: function() {return true;},
    supportsProperty: function() {return true;},
    supportsValue: function() {return true;},
    setup: function() {},
    nonInterpolationExpectations: function(from, to) {
      return expectFlip(from, to, 0.5);
    },
    notAnimatableExpectations: function(from, to, underlying) {
      return expectFlip(underlying, underlying, -Infinity);
    },
    interpolate: function(property, from, to, at, target) {
      var id = cssAnimationsData.nextID++;
      if (!cssAnimationsData.sharedStyle) {
        cssAnimationsData.sharedStyle = createElement(document.body, 'style');
      }
      cssAnimationsData.sharedStyle.textContent += '' +
        '@keyframes animation' + id + ' {' +
          (isNeutralKeyframe(from) ? '' : `from {${property}:${from};}`) +
          (isNeutralKeyframe(to) ? '' : `to {${property}:${to};}`) +
        '}';
      target.style.animationName = 'animation' + id;
      target.style.animationDuration = '100s';
      target.style.animationDelay = '-50s';
      target.style.animationTimingFunction = createEasing(at);
    },
  };

  var cssTransitionsInterpolation = {
    name: 'CSS Transitions',
    isSupported: function() {return true;},
    supportsProperty: function() {return true;},
    supportsValue: function() {return true;},
    setup: function(property, from, target) {
      target.style.setProperty(property, isNeutralKeyframe(from) ? '' : from);
    },
    nonInterpolationExpectations: function(from, to) {
      return expectFlip(from, to, -Infinity);
    },
    notAnimatableExpectations: function(from, to, underlying) {
      return expectFlip(from, to, -Infinity);
    },
    interpolate: function(property, from, to, at, target, behavior) {
      // Force a style recalc on target to set the 'from' value.
      getComputedStyle(target).getPropertyValue(property);
      target.style.transitionDuration = '100s';
      target.style.transitionDelay = '-50s';
      target.style.transitionTimingFunction = createEasing(at);
      target.style.transitionProperty = property;
      if (behavior) {
        target.style.transitionBehavior = behavior;
      }
      target.style.setProperty(property, isNeutralKeyframe(to) ? '' : to);
    },
  };

  var cssTransitionAllInterpolation = {
    name: 'CSS Transitions with transition: all',
    isSupported: function() {return true;},
    // The 'all' value doesn't cover custom properties.
    supportsProperty: function(property) {return property.indexOf('--') !== 0;},
    supportsValue: function() {return true;},
    setup: function(property, from, target) {
      target.style.setProperty(property, isNeutralKeyframe(from) ? '' : from);
    },
    nonInterpolationExpectations: function(from, to) {
      return expectFlip(from, to, -Infinity);
    },
    notAnimatableExpectations: function(from, to, underlying) {
      return expectFlip(from, to, -Infinity);
    },
    interpolate: function(property, from, to, at, target, behavior) {
      // Force a style recalc on target to set the 'from' value.
      getComputedStyle(target).getPropertyValue(property);
      target.style.transitionDuration = '100s';
      target.style.transitionDelay = '-50s';
      target.style.transitionTimingFunction = createEasing(at);
      target.style.transitionProperty = 'all';
      if (behavior) {
        target.style.transitionBehavior = behavior;
      }
      target.style.setProperty(property, isNeutralKeyframe(to) ? '' : to);
    },
  };

  var cssTransitionsInterpolationAllowDiscrete = {
    name: 'CSS Transitions with transition-behavior:allow-discrete',
    isSupported: function() {return true;},
    supportsProperty: function() {return true;},
    supportsValue: function() {return true;},
    setup: function(property, from, target) {
      target.style.setProperty(property, isNeutralKeyframe(from) ? '' : from);
    },
    nonInterpolationExpectations: function(from, to) {
      return expectFlip(from, to, 0.5);
    },
    notAnimatableExpectations: function(from, to, underlying) {
      return expectFlip(from, to, -Infinity);
    },
    interpolate: function(property, from, to, at, target, behavior) {
      // Force a style recalc on target to set the 'from' value.
      getComputedStyle(target).getPropertyValue(property);
      target.style.transitionDuration = '100s';
      target.style.transitionDelay = '-50s';
      target.style.transitionTimingFunction = createEasing(at);
      target.style.transitionProperty = property;
      target.style.transitionBehavior = 'allow-discrete';
      target.style.setProperty(property, isNeutralKeyframe(to) ? '' : to);
    },
  };

  var cssTransitionAllInterpolationAllowDiscrete = {
    name: 'CSS Transitions with transition-property:all and transition-behavor:allow-discrete',
    isSupported: function() {return true;},
    // The 'all' value doesn't cover custom properties.
    supportsProperty: function(property) {return property.indexOf('--') !== 0;},
    supportsValue: function() {return true;},
    setup: function(property, from, target) {
      target.style.setProperty(property, isNeutralKeyframe(from) ? '' : from);
    },
    nonInterpolationExpectations: function(from, to) {
      return expectFlip(from, to, 0.5);
    },
    notAnimatableExpectations: function(from, to, underlying) {
      return expectFlip(from, to, -Infinity);
    },
    interpolate: function(property, from, to, at, target, behavior) {
      // Force a style recalc on target to set the 'from' value.
      getComputedStyle(target).getPropertyValue(property);
      target.style.transitionDuration = '100s';
      target.style.transitionDelay = '-50s';
      target.style.transitionTimingFunction = createEasing(at);
      target.style.transitionProperty = 'all';
      target.style.transitionBehavior = 'allow-discrete';
      target.style.setProperty(property, isNeutralKeyframe(to) ? '' : to);
    },
  };

  var webAnimationsInterpolation = {
    name: 'Web Animations',
    isSupported: function() {return 'animate' in Element.prototype;},
    supportsProperty: function(property) {return true;},
    supportsValue: function(value) {return value !== '';},
    setup: function() {},
    nonInterpolationExpectations: function(from, to) {
      return expectFlip(from, to, 0.5);
    },
    notAnimatableExpectations: function(from, to, underlying) {
      return expectFlip(underlying, underlying, -Infinity);
    },
    interpolate: function(property, from, to, at, target) {
      this.interpolateComposite(property, from, 'replace', to, 'replace', at, target);
    },
    interpolateComposite: function(property, from, fromComposite, to, toComposite, at, target) {
      // This case turns into a test error later on.
      if (!this.isSupported())
        return;

      // Convert standard properties to camelCase.
      if (!property.startsWith('--')) {
        for (var i = property.length - 2; i > 0; --i) {
          if (property[i] === '-') {
            property = property.substring(0, i) + property[i + 1].toUpperCase() + property.substring(i + 2);
          }
        }
        if (property === 'offset') {
          property = 'cssOffset';
        } else if (property === 'float') {
          property = 'cssFloat';
        }
      }
      var keyframes = [];
      if (!isNeutralKeyframe(from)) {
        keyframes.push({
          offset: 0,
          composite: fromComposite,
          [property]: from,
        });
      }
      if (!isNeutralKeyframe(to)) {
        keyframes.push({
          offset: 1,
          composite: toComposite,
          [property]: to,
        });
      }
      var animation = target.animate(keyframes, {
        fill: 'forwards',
        duration: 100 * 1000,
        easing: createEasing(at),
      });
      animation.pause();
      animation.currentTime = 50 * 1000;
    },
  };

  function expectFlip(from, to, flipAt) {
    return [-0.3, 0, 0.3, 0.5, 0.6, 1, 1.5].map(function(at) {
      return {
        at: at,
        expect: at < flipAt ? from : to
      };
    });
  }

  // Constructs a timing function which produces 'y' at x = 0.5
  function createEasing(y) {
    if (y == 0) {
      return 'steps(1, end)';
    }
    if (y == 1) {
      return 'steps(1, start)';
    }
    if (y == 0.5) {
      return 'linear';
    }
    // Approximate using a bezier.
    var b = (8 * y - 1) / 6;
    return 'cubic-bezier(0, ' + b + ', 1, ' + b + ')';
  }

  function createElement(parent, tag, text) {
    var element = document.createElement(tag || 'div');
    element.textContent = text || '';
    parent.appendChild(element);
    return element;
  }

  function createTargetContainer(parent, className) {
    var targetContainer = createElement(parent);
    targetContainer.classList.add('container');
    var template = document.querySelector('#target-template');
    if (template) {
      targetContainer.appendChild(template.content.cloneNode(true));
    }
    var target = targetContainer.querySelector('.target') || targetContainer;
    target.classList.add('target', className);
    target.parentElement.classList.add('parent');
    targetContainer.target = target;
    return targetContainer;
  }

  function roundNumbers(value) {
    return value.
        // Round numbers to two decimal places.
        replace(/-?\d*\.\d+(e-?\d+)?/g, function(n) {
          return (parseFloat(n).toFixed(2)).
              replace(/\.\d+/, function(m) {
                return m.replace(/0+$/, '');
              }).
              replace(/\.$/, '').
              replace(/^-0$/, '0');
        });
  }

  var anchor = document.createElement('a');
  function sanitizeUrls(value) {
    var matches = value.match(/url\("([^#][^\)]*)"\)/g);
    if (matches !== null) {
      for (var i = 0; i < matches.length; ++i) {
        var url = /url\("([^#][^\)]*)"\)/g.exec(matches[i])[1];
        anchor.href = url;
        anchor.pathname = '...' + anchor.pathname.substring(anchor.pathname.lastIndexOf('/'));
        value = value.replace(matches[i], 'url(' + anchor.href + ')');
      }
    }
    return value;
  }

  function normalizeValue(value) {
    return roundNumbers(sanitizeUrls(value)).
        // Place whitespace between tokens.
        replace(/([\w\d.]+|[^\s])/g, '$1 ').
        replace(/\s+/g, ' ');
  }

  function stringify(text) {
    if (!text.includes("'")) {
      return `'${text}'`;
    }
    return `"${text.replace('"', '\\"')}"`;
  }

  function keyframeText(keyframe) {
    return isNeutralKeyframe(keyframe) ? 'neutral' : `[${keyframe}]`;
  }

  function keyframeCode(keyframe) {
    return isNeutralKeyframe(keyframe) ? 'neutralKeyframe' : `${stringify(keyframe)}`;
  }

  function createInterpolationTestTargets(interpolationMethod, interpolationMethodContainer, interpolationTest) {
    var property = interpolationTest.options.property;
    var from = interpolationTest.options.from;
    var to = interpolationTest.options.to;
    let underlying = interpolationTest.options.underlying;
    var comparisonFunction = interpolationTest.options.comparisonFunction;
    var behavior = interpolationTest.options.behavior;

    if ((interpolationTest.options.method && interpolationTest.options.method != interpolationMethod.name)
      || !interpolationMethod.supportsProperty(property)
      || !interpolationMethod.supportsValue(from)
      || !interpolationMethod.supportsValue(to)) {
      return;
    }

    var testText = `${interpolationMethod.name}: property <${property}> from ${keyframeText(from)} to ${keyframeText(to)}`;
    var testContainer = createElement(interpolationMethodContainer, 'div');
    createElement(testContainer);
    var expectations = interpolationTest.expectations;
    var applyUnderlying = false;
    if (expectations === expectNoInterpolation) {
      expectations = interpolationMethod.nonInterpolationExpectations(from, to);
    } else if (expectations === expectNotAnimatable) {
      expectations = interpolationMethod.notAnimatableExpectations(from, to, interpolationTest.options.underlying);
      applyUnderlying = true;
    } else if (interpolationTest.options[interpolationMethod.name]) {
      expectations = interpolationTest.options[interpolationMethod.name];
    }

    // Setup a standard equality function if an override is not provided.
    if (!comparisonFunction) {
      comparisonFunction = (actual, expected) => {
        assert_equals(normalizeValue(actual), normalizeValue(expected));
      };
    }

    return expectations.map(function(expectation) {
      var actualTargetContainer = createTargetContainer(testContainer, 'actual');
      var expectedTargetContainer = createTargetContainer(testContainer, 'expected');
      var expectedProperties = expectation.option || expectation.expect;
      if (typeof expectedProperties !== "object") {
        expectedProperties = {[property]: expectedProperties};
      }
      var target = actualTargetContainer.target;
      if (applyUnderlying) {
        assert_true(typeof underlying !== 'undefined', '\'underlying\' value must be provided');
        target.style.setProperty(property, underlying);
      }
      interpolationMethod.setup(property, from, target);
      target.interpolate = function() {
        interpolationMethod.interpolate(property, from, to, expectation.at, target, behavior);
      };
      target.measure = function() {
        for (var [expectedProp, expectedStr] of Object.entries(expectedProperties)) {
          if (!isNeutralKeyframe(expectedStr)) {
            expectedTargetContainer.target.style.setProperty(expectedProp, expectedStr);
          }
          var expectedValue = getComputedStyle(expectedTargetContainer.target).getPropertyValue(expectedProp);
          let testName = `${testText} at (${expectation.at}) should be [${sanitizeUrls(expectedStr)}]`;
          if (property !== expectedProp) {
            testName += ` for <${expectedProp}>`;
          }
          test(function() {
            assert_true(interpolationMethod.isSupported(), `${interpolationMethod.name} should be supported`);

            if (from && from !== neutralKeyframe) {
              assert_true(CSS.supports(property, from), '\'from\' value should be supported');
            }
            if (to && to !== neutralKeyframe) {
              assert_true(CSS.supports(property, to), '\'to\' value should be supported');
            }
            if (typeof underlying !== 'undefined') {
              assert_true(CSS.supports(property, underlying), '\'underlying\' value should be supported');
            }

            comparisonFunction(
                getComputedStyle(target).getPropertyValue(expectedProp),
                expectedValue);
          }, testName);
        }
      };
      return target;
    });
  }

  function createCompositionTestTargets(compositionContainer, compositionTest) {
    var options = compositionTest.options;
    var property = options.property;
    var underlying = options.underlying;
    var comparisonFunction = options.comparisonFunction;
    var from = options.accumulateFrom || options.addFrom || options.replaceFrom;
    var to = options.accumulateTo || options.addTo || options.replaceTo;
    var fromComposite = 'accumulateFrom' in options ? 'accumulate' : 'addFrom' in options ? 'add' : 'replace';
    var toComposite = 'accumulateTo' in options ? 'accumulate' : 'addTo' in options ? 'add' : 'replace';
    const invalidFrom = 'addFrom' in options === 'replaceFrom' in options
        && 'addFrom' in options === 'accumulateFrom' in options;
    const invalidTo = 'addTo' in options === 'replaceTo' in options
        && 'addTo' in options === 'accumulateTo' in options;
    if (invalidFrom || invalidTo) {
      test(function() {
        assert_false(invalidFrom, 'Exactly one of accumulateFrom, addFrom, or replaceFrom must be specified');
        assert_false(invalidTo, 'Exactly one of accumulateTo, addTo, or replaceTo must be specified');
      }, `Composition tests must have valid setup`);
    }

    var testText = `Compositing: property <${property}> underlying [${underlying}] from ${fromComposite} [${from}] to ${toComposite} [${to}]`;
    var testContainer = createElement(compositionContainer, 'div');
    createElement(testContainer);

    // Setup a standard equality function if an override is not provided.
    if (!comparisonFunction) {
      comparisonFunction = (actual, expected) => {
        assert_equals(normalizeValue(actual), normalizeValue(expected));
      };
    }

    return compositionTest.expectations.map(function(expectation) {
      var actualTargetContainer = createTargetContainer(testContainer, 'actual');
      var expectedTargetContainer = createTargetContainer(testContainer, 'expected');
      var expectedStr = expectation.option || expectation.expect;
      if (!isNeutralKeyframe(expectedStr)) {
        expectedTargetContainer.target.style.setProperty(property, expectedStr);
      }
      var target = actualTargetContainer.target;
      target.style.setProperty(property, underlying);
      target.interpolate = function() {
        webAnimationsInterpolation.interpolateComposite(property, from, fromComposite, to, toComposite, expectation.at, target);
      };
      target.measure = function() {
        var expectedValue = getComputedStyle(expectedTargetContainer.target).getPropertyValue(property);
        test(function() {

          if (from && from !== neutralKeyframe) {
            assert_true(CSS.supports(property, from), '\'from\' value should be supported');
          }
          if (to && to !== neutralKeyframe) {
            assert_true(CSS.supports(property, to), '\'to\' value should be supported');
          }
          if (typeof underlying !== 'undefined') {
            assert_true(CSS.supports(property, underlying), '\'underlying\' value should be supported');
          }

          comparisonFunction(
              getComputedStyle(target).getPropertyValue(property),
              expectedValue);
        }, `${testText} at (${expectation.at}) should be [${sanitizeUrls(expectedStr)}]`);
      };
      return target;
    });
  }



  function createTestTargets(interpolationMethods, interpolationTests, compositionTests, container) {
    var targets = [];
    for (var interpolationMethod of interpolationMethods) {
      var interpolationMethodContainer = createElement(container);
      for (var interpolationTest of interpolationTests) {
        if(!interpolationTest.options.target_names ||
           interpolationTest.options.target_names.includes(interpolationMethod.name)) {
            [].push.apply(targets, createInterpolationTestTargets(interpolationMethod, interpolationMethodContainer, interpolationTest));
          }
      }
    }
    var compositionContainer = createElement(container);
    for (var compositionTest of compositionTests) {
      [].push.apply(targets, createCompositionTestTargets(compositionContainer, compositionTest));
    }
    return targets;
  }

  function test_no_interpolation(options) {
    test_interpolation(options, expectNoInterpolation);
  }
  function test_not_animatable(options) {
    test_interpolation(options, expectNotAnimatable);
  }
  function create_tests(addAllowDiscreteTests) {
    var interpolationMethods = [
      cssTransitionsInterpolation,
      cssTransitionAllInterpolation,
      cssAnimationsInterpolation,
      webAnimationsInterpolation,
    ];
    if (addAllowDiscreteTests) {
      interpolationMethods = [
        cssTransitionsInterpolationAllowDiscrete,
        cssTransitionAllInterpolationAllowDiscrete,
      ].concat(interpolationMethods);
    }
    var container = createElement(document.body);
    var targets = createTestTargets(interpolationMethods, interpolationTests, compositionTests, container);
    // Separate interpolation and measurement into different phases to avoid O(n^2) of the number of targets.
    for (var target of targets) {
      target.interpolate();
    }
    for (var target of targets) {
      target.measure();
    }
    container.remove();
  }

  function test_interpolation(options, expectations) {
    interpolationTests.push({options, expectations});
    create_tests(expectations === expectNoInterpolation || expectations === expectNotAnimatable);
    interpolationTests = [];
  }
  function test_composition(options, expectations) {
    compositionTests.push({options, expectations});
    create_tests();
    compositionTests = [];
  }
  window.test_interpolation = test_interpolation;
  window.test_no_interpolation = test_no_interpolation;
  window.test_not_animatable = test_not_animatable;
  window.test_composition = test_composition;
  window.neutralKeyframe = neutralKeyframe;
  window.roundNumbers = roundNumbers;
  window.normalizeValue = normalizeValue;
})();
