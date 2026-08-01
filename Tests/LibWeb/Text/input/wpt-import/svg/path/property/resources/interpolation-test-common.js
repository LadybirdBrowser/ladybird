'use strict';
function test_interpolation(settings, expectations) {

  test(function(){
    assert_true(CSS.supports(settings.property, settings.from), 'Value "' + settings.from + '" is supported by ' + settings.property);
    assert_true(CSS.supports(settings.property, settings.to), 'Value "' + settings.to + '" is supported by ' + settings.property);
  }, '"' + settings.from + '" and "' + settings.to + '" are valid ' + settings.property + ' values');

  const container = document.getElementById('container');
  for (let i = 0; i < expectations.length; ++i) {
    const progress = expectations[i].at;
    const expectation = expectations[i].expect;
    const animationId = 'anim' + i;
    const targetId = 'target' + i;
    const referenceId = 'reference' + i;

    test(function(){
      assert_true(CSS.supports(settings.property, expectation), 'Value "' + expectation + '" is supported by ' + settings.property);

      const target = document.createElementNS('http://www.w3.org/2000/svg', 'g');
      target.id = targetId;
      container.appendChild(target);

      const reference = document.createElementNS('http://www.w3.org/2000/svg', 'g');
      reference.id = referenceId;
      container.appendChild(reference);

      assert_equals(getComputedStyle(target)[settings.property], getComputedStyle(reference)[settings.property]);

      // Create an animation of length 2s that starts at -1s so the current time of 0s is
      // exactly halfway through the animation. A cubic bezier timing function is used that
      // evaluates to |progress| at the current time (halfway through the animation).

      // Cubic bezier evaluates to |progress| at 50%.
      const y = (8 * progress - 1) / 6;
      const timing_function = 'cubic-bezier(0, ' + y + ', 1, ' + y + ')';

      const stylesheet = document.createElementNS('http://www.w3.org/2000/svg', 'style');
      stylesheet.textContent =
        '#' + targetId + ' {\n' +
        '  animation: 2s ' + timing_function + ' -1s paused ' + animationId + ';\n' +
        '}\n' +
        '@keyframes ' + animationId + ' {\n' +
        '  0% { ' + settings.property + ': ' + settings.from + '; }\n' +
        '  100% { ' + settings.property + ': ' + settings.to + '; }\n' +
        '}\n' +
        '#' + referenceId + ' {\n' +
        '  ' + settings.property + ': ' + expectation + ';\n' +
        '}\n';
      container.appendChild(stylesheet);

      assert_equals(getComputedStyle(target)[settings.property], getComputedStyle(reference)[settings.property]);
      assert_equals(getComputedStyle(target)[settings.property], expectation);

      container.removeChild(target);
      container.removeChild(reference);
      container.removeChild(stylesheet);
    }, 'Animation between "' + settings.from + '" and "' + settings.to + '" at progress ' + progress);
  }
}

function test_no_interpolation(settings) {
  const expectFrom = [-1, 0, 0.125].map(function (progress) {
    return {at: progress, expect: settings.from};
  });
  const expectTo = [0.875, 1, 2].map(function (progress) {
    return {at: progress, expect: settings.to};
  });

  test_interpolation(settings, expectFrom.concat(expectTo));
}
