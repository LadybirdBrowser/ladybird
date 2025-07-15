'use strict';

const gEasingTests = [
  {
    desc: 'step-start function',
    easing: 'step-start',
    easingFunction: stepStart(1),
    serialization: 'steps(1, start)'
  },
  {
    desc: 'steps(1, start) function',
    easing: 'steps(1, start)',
    easingFunction: stepStart(1)
  },
  {
    desc: 'steps(2, start) function',
    easing: 'steps(2, start)',
    easingFunction: stepStart(2)
  },
  {
    desc: 'step-end function',
    easing: 'step-end',
    easingFunction: stepEnd(1),
    serialization: 'steps(1)'
  },
  {
    desc: 'steps(1) function',
    easing: 'steps(1)',
    easingFunction: stepEnd(1)
  },
  {
    desc: 'steps(1, end) function',
    easing: 'steps(1, end)',
    easingFunction: stepEnd(1),
    serialization: 'steps(1)'
  },
  {
    desc: 'steps(2, end) function',
    easing: 'steps(2, end)',
    easingFunction: stepEnd(2),
    serialization: 'steps(2)'
  },
  {
    desc: 'linear function',
    easing: 'linear', // cubic-bezier(0, 0, 1.0, 1.0)
    easingFunction: cubicBezier(0, 0, 1.0, 1.0)
  },
  {
    desc: 'ease function',
    easing: 'ease', // cubic-bezier(0.25, 0.1, 0.25, 1.0)
    easingFunction: cubicBezier(0.25, 0.1, 0.25, 1.0)
  },
  {
    desc: 'ease-in function',
    easing: 'ease-in', // cubic-bezier(0.42, 0, 1.0, 1.0)
    easingFunction: cubicBezier(0.42, 0, 1.0, 1.0)
  },
  {
    desc: 'ease-in-out function',
    easing: 'ease-in-out', // cubic-bezier(0.42, 0, 0.58, 1.0)
    easingFunction: cubicBezier(0.42, 0, 0.58, 1.0)
  },
  {
    desc: 'ease-out function',
    easing: 'ease-out', // cubic-bezier(0, 0, 0.58, 1.0)
    easingFunction: cubicBezier(0, 0, 0.58, 1.0)
  },
  {
    desc: 'easing function which produces values greater than 1',
    easing: 'cubic-bezier(0, 1.5, 1, 1.5)',
    easingFunction: cubicBezier(0, 1.5, 1, 1.5)
  },
  {
    desc: 'easing function which produces values less than 1',
    easing: 'cubic-bezier(0, -0.5, 1, -0.5)',
    easingFunction: cubicBezier(0, -0.5, 1, -0.5)
  }
];

const gEasingParsingTests = [
  ['linear', 'linear'],
  ['ease-in-out', 'ease-in-out'],
  ['Ease\\2d in-out', 'ease-in-out'],
  ['ease /**/', 'ease'],
];

const gInvalidEasings = [
  '',
  '7',
  'test',
  'initial',
  'inherit',
  'unset',
  'unrecognized',
  'var(--x)',
  'ease-in-out, ease-out',
  'cubic-bezier(1.1, 0, 1, 1)',
  'cubic-bezier(0, 0, 1.1, 1)',
  'cubic-bezier(-0.1, 0, 1, 1)',
  'cubic-bezier(0, 0, -0.1, 1)',
  'cubic-bezier(0.1, 0, 4, 0.4)',
  'steps(-1, start)',
  'steps(0.1, start)',
  'steps(3, nowhere)',
  'steps(-3, end)',
  'function (a){return a}',
  'function (x){return x}',
  'function(x, y){return 0.3}',
];

// Easings that should serialize to the same string
const gRoundtripEasings = [
  'ease',
  'linear',
  'ease-in',
  'ease-out',
  'ease-in-out',
  'cubic-bezier(0.1, 5, 0.23, 0)',
  'steps(3, start)',
  'steps(3)',
];
