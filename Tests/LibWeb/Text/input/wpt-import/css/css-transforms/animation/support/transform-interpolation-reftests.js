'use strict';

// Each test is an array of [endpoint, midpoint, endpoint] and tests
// whether the endpoints interpolate to the same visual state as the midpoint
const transformTests = {
  translate: [
    ['translateX(0px)', 'translateX(25px)', 'translateX(50px)'],
    ['translateY(0px)', 'translateY(25px)', 'translateY(50px)'],
    ['translateX(0%)', 'translateX(25%)', 'translateX(50%)'],
    ['translateY(0%)', 'translateY(25%)', 'translateY(50%)'],
    ['translateX(50%)', 'translate(25%, 25%)', 'translateY(50%)'],
    ['translateX(50%)', 'translate(25%, 25px)', 'translateY(50px)'],
    ['translateX(50px)', 'translateX(calc(25px + 25%))', 'translateX(50%)']
  ],
  translateEm: [
    ['translateX(0em)', 'translateX(2em)', 'translateX(4em)'],
    ['translateX(-50px)', 'translateX(calc(2em - 25px))', 'translateX(4em)'],
    ['translateX(50%)', 'translateX(calc(25% - 2em))', 'translateX(-4em)']
  ],
  rotate: [
    // Rotation about named-axis.
    ['rotate(30deg)', 'rotate(60deg)', 'rotate(90deg)'],
    ['rotateX(30deg)', 'rotateX(60deg)', 'rotateX(90deg)'],
    ['rotateY(30deg)', 'rotateY(60deg)', 'rotateY(90deg)'],
    ['rotate(30deg)', 'rotate(60deg)', 'rotateZ(90deg)'],
    ['rotate(0deg)', 'rotate(180deg)', 'rotate(360deg)'],
    // Common axis rotations.
    ['rotate3d(7, 8, 9, 0deg)', 'rotate3d(7, 8, 9, 45deg)', 'rotate3d(7, 8, 9, 90deg)'],
    ['rotate3d(1, 2, 3, 0deg)', 'rotate3d(3, 6, 9, 45deg)', 'rotate3d(2, 4, 6, 90deg)'],
    // Axis is arbitrary if angle is zero. Use non-zero rotation to determine
    // the rotation axis.
    ['rotateX(0deg)', 'rotate(45deg)', 'rotate(90deg)'],
    ['rotateX(90deg)', 'rotateX(45deg)', 'rotate(0deg)']
  ],
  rotateSlerp: [
    // First endpoint is the same rotation as rotateZ(0deg) but triggers SLERP
    ['rotateX(360deg)', 'rotateZ(45deg)', 'rotateZ(90deg)'],
    // Interpolation with inverse. Second case is a common-axis case, but
    // included here to group it with its equivalent SLERP test.
    ['rotate(45deg)', 'rotate(0deg)', 'rotate3d(0, 0, -1, 45deg)'],
    ['rotate(45deg)', 'rotate(0deg)', 'rotate(-45deg)'],
    // Interpolate axis and angle of rotation.
    // 70.5288deg = acos(1/3).
    ['rotateX(90deg)', 'rotate3d(1, 1, 0, 70.5288deg)', 'rotateY(90deg)'],
    // Not nice analytical solution for this last one.
    // (1, 1, 0, 90deg) --> (x, y, z, w) = (1/2, 1/2, 0, 1/root2)
    // (0, 1, 1, 180deg) --> (x, y, z, w) = (0, 1/root2, 1/root2, 0)
    // Trace of the "to" transformation matrix is -1. Requires special handling
    // to ensure correctness of the quaternion.
    // SLERP @0.5: (x, y, z, w) = (0.30389062997686395,
    //                             0.7336568918027127,
    //                             0.4297662618258487,
    //                             0.4297662618258487)
    // --> rotate3d(0.3365568, 0.8125199, 0.4759632, 129.094547486deg)
    ['rotate3d(1, 1, 0, 90deg)',
     'rotate3d(0.3365568, 0.8125199, 0.4759632, 129.094547486deg)',
     'rotate3d(0, 1, 1, 180deg)'],
  ],
  scale: [
    ['scaleX(0.5)', 'scaleX(0.75)', 'scaleX(1)'],
    ['scaleY(0.5)', 'scaleY(0.75)', 'scaleY(1)'],
    ['scale(0.5)', 'scale(0.75)', 'scale(1)'],
    ['scaleX(0.5)', 'scale(0.75)', 'scaleY(0.5)'],
    ['scale3d(0.5, 1, 2)', 'scale3d(0.75, 0.75, 3)', 'scale3d(1, 0.5, 4)']
  ],
  skew: [
    ['skewX(0deg)', 'skewX(30deg)', 'skewX(60deg)'],
    ['skewY(0deg)', 'skewY(30deg)', 'skewY(60deg)'],
    ['skew(60deg, 0deg)', 'skew(30deg, 30deg)', 'skew(0deg, 60deg)'],
    ['skewX(0deg) rotate(0deg)', 'skewX(0deg) rotate(180deg)', 'skewX(0deg) rotate(360deg)'],
    ['skewX(0deg) rotate(0deg)', 'matrix(1, 0, 0, 1, 0, 0)', 'skewY(0deg) rotate(360deg)']
  ],
  matrix: [
    // matched matrix parameters do not collapse the values after them
    ['matrix(1,0,0,1,0,0) rotate(0deg)', 'matrix(1.5,0,0,1.5,0,0) rotate(180deg)', 'matrix(2,0,0,2,0,0) rotate(360deg)']
  ],
  perspective: [
    // Since perspective doesn't do anything on its own, we need to
    // combine it with a transform that does.
    ['perspective(none) translateZ(15px)', 'perspective(none) translateZ(15px)', 'perspective(none) translateZ(15px)'],
    ['perspective(100px) translateZ(50px)', 'perspective(200px) translateZ(50px)', 'perspective(none) translateZ(50px)'],
    ['perspective(none) translateZ(15px)', 'perspective(50px) translateZ(15px)', 'perspective(25px) translateZ(15px)'],
    ['perspective(100px) translateZ(15px)', 'perspective(40px) translateZ(15px)', 'perspective(25px) translateZ(15px)'],

    // Test that perspective is clamped to 1px.
    ['perspective(0.1px) translateZ(0.25px)', 'perspective(1px) translateZ(0.25px)', 'perspective(0.1px) translateZ(0.25px)'],
    ['perspective(0px) translateZ(0.25px)', 'perspective(1px) translateZ(0.25px)', 'perspective(0px) translateZ(0.25px)'],
    ['perspective(0px) translateZ(0.5px)', 'perspective(1.5px) translateZ(0.5px)', 'perspective(3px) translateZ(0.5px)'],
    { test: ['perspective(10px) translateZ(0.5px)', 'translateZ(0.5px)', 'perspective(1px) translateZ(0.5px)'], midpoint: -1 },
    { test: ['perspective(1px) translateZ(0.5px)', 'perspective(1px) translateZ(0.5px)', 'perspective(10px) translateZ(0.5px)'], midpoint: -1 }
  ]
};

// Initial setup, which includes properties that will be overridden to
// test invalidation.
function initialStyle(div) {
  div.style.width = '180px';
  div.style.height = '150px';
  div.style.margin = '50px';
  div.style.borderLeft = 'solid 40px blue';
  div.style.backgroundColor = 'green';
  div.style.willChange = 'transform';
  div.style.fontSize = '30px';
}

function finalStyle(div) {
  div.style.width = '80px';
  div.style.height = '80px';
  div.style.fontSize = '15px';
}

function styleBody(){
  let body = document.body;
  body.style.display = 'flex';
  body.style.flexDirection = 'row';
  body.style.flexWrap = 'wrap';
}

// Simulate a static image at 50% progress with a running animation.
// The easing curve has zero slope and curvature at its midpoint of 50% -> 50%.
// The timing values are chosen so as so that a delay of up to 10s will not
// cause a visual change.
const duration = 1e9;
const midpointOptions = {
  easing: 'cubic-bezier(0,1,1,0)',
  duration: duration,
  delay: -duration/2
};

// Constant-valued animation using the ending keyframe's value.
const referenceOptions = {
  easing: 'steps(1, jump-start)',
  duration: duration,
  delay: -duration/2
}

// Similar to midpointOptions, but to produce the interpolation result
// at -1 instead of the interpolation result at 0.5.  This easing curve
// has zero slope at its midpoint of -100% (though does have curvature).
const negoneOptions = {
  easing: 'cubic-bezier(0,-1,1,-2)',
  duration: duration,
  delay: -duration/2
};

// Indices to unpack a test case, which is in the format
// [start, midpoint, end]
const startIndex = 0;
const midIndex = 1;
const endIndex = 2;

async function createTests(tests) {
  styleBody();
  for (const obj of tests) {
    let test = ("test" in obj) ? obj.test : obj;
    let midpoint = ("midpoint" in obj) ? obj.midpoint : 0.5;
    let options;
    if (midpoint == 0.5) {
      options = midpointOptions;
    } else if (midpoint == -1) {
      options = negoneOptions;
    } else {
      document.appendChild(document.createTextNode("unexpected midpoint " + midpoint));
    }
    let div = document.createElement('div');
    document.body.appendChild(div);
    initialStyle(div);
    var anim =
      div.animate({transform: [test[startIndex], test[endIndex]]}, options);
    await anim.ready;
    finalStyle(div);  // Change size to test invalidation.
  }

  await new Promise(requestAnimationFrame);
  await new Promise(requestAnimationFrame);
  takeScreenshot();
}

// Create references using a constant-valued animation  to avoid rounding and
// anti-aliasing differences between animated and non-animated pathways.
async function createRefs(tests) {
  styleBody();
  for (const obj of tests) {
    let test = ("test" in obj) ? obj.test : obj;
    let div = document.createElement('div');
    document.body.appendChild(div);
    initialStyle(div);
    finalStyle(div);
    var anim = div.animate(
        {transform: ['none', test[midIndex]]},
        referenceOptions);
    await anim.ready;
  }

  await new Promise(requestAnimationFrame);
  await new Promise(requestAnimationFrame);
  takeScreenshot();
}

