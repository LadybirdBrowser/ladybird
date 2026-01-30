// Use a power of two to eliminate round-off when converting frames to time and
// vice versa.
let sampleRate = 32768;

// How many panner nodes to create for the test.
let nodesToCreate = 100;

// Time step when each panner node starts.  Make sure it starts on a frame
// boundary.
let timeStep = Math.floor(0.001 * sampleRate) / sampleRate;

// Make sure we render long enough to get all of our nodes.
let renderLengthSeconds = timeStep * (nodesToCreate + 1);

// Length of an impulse signal.
let pulseLengthFrames = Math.round(timeStep * sampleRate);

// Globals to make debugging a little easier.
let context;
let impulse;
let bufferSource;
let panner;
let position;
let time;

// For the record, these distance formulas were taken from the OpenAL
// spec
// (http://connect.creativelabs.com/openal/Documentation/OpenAL%201.1%20Specification.pdf),
// not the code.  The Web Audio spec follows the OpenAL formulas.

function linearDistance(panner, x, y, z) {
  let distance = Math.sqrt(x * x + y * y + z * z);
  distance = Math.min(distance, panner.maxDistance);
  let rolloff = panner.rolloffFactor;
  let gain =
      (1 -
       rolloff * (distance - panner.refDistance) /
           (panner.maxDistance - panner.refDistance));

  return gain;
}

function inverseDistance(panner, x, y, z) {
  let distance = Math.sqrt(x * x + y * y + z * z);
  distance = Math.min(distance, panner.maxDistance);
  let rolloff = panner.rolloffFactor;
  let gain = panner.refDistance /
      (panner.refDistance + rolloff * (distance - panner.refDistance));

  return gain;
}

function exponentialDistance(panner, x, y, z) {
  let distance = Math.sqrt(x * x + y * y + z * z);
  distance = Math.min(distance, panner.maxDistance);
  let rolloff = panner.rolloffFactor;
  let gain = Math.pow(distance / panner.refDistance, -rolloff);

  return gain;
}

// Map the distance model to the function that implements the model
let distanceModelFunction = {
  'linear': linearDistance,
  'inverse': inverseDistance,
  'exponential': exponentialDistance
};

function createGraph(context, distanceModel, nodeCount) {
  bufferSource = new Array(nodeCount);
  panner = new Array(nodeCount);
  position = new Array(nodeCount);
  time = new Array(nodesToCreate);

  impulse = createImpulseBuffer(context, pulseLengthFrames);

  // Create all the sources and panners.
  //
  // We MUST use the EQUALPOWER panning model so that we can easily
  // figure out the gain introduced by the panner.
  //
  // We want to stay in the middle of the panning range, which means
  // we want to stay on the z-axis.  If we don't, then the effect of
  // panning model will be much more complicated.  We're not testing
  // the panner, but the distance model, so we want the panner effect
  // to be simple.
  //
  // The panners are placed at a uniform intervals between the panner
  // reference distance and the panner max distance.  The source is
  // also started at regular intervals.
  for (let k = 0; k < nodeCount; ++k) {
    bufferSource[k] = context.createBufferSource();
    bufferSource[k].buffer = impulse;

    panner[k] = context.createPanner();
    panner[k].panningModel = 'equalpower';
    panner[k].distanceModel = distanceModel;

    let distanceStep =
        (panner[k].maxDistance - panner[k].refDistance) / nodeCount;
    position[k] = distanceStep * k + panner[k].refDistance;
    panner[k].setPosition(0, 0, position[k]);

    bufferSource[k].connect(panner[k]);
    panner[k].connect(context.destination);

    time[k] = k * timeStep;
    bufferSource[k].start(time[k]);
  }
}

// distanceModel should be the distance model string like
// "linear", "inverse", or "exponential".
function createTestAndRun(context, distanceModel, should) {
  // To test the distance models, we create a number of panners at
  // uniformly spaced intervals on the z-axis.  Each of these are
  // started at equally spaced time intervals.  After rendering the
  // signals, we examine where each impulse is located and the
  // attenuation of the impulse.  The attenuation is compared
  // against our expected attenuation.

  createGraph(context, distanceModel, nodesToCreate);

  return context.startRendering().then(
      buffer => checkDistanceResult(buffer, distanceModel, should));
}

// The gain caused by the EQUALPOWER panning model, if we stay on the
// z axis, with the default orientations.
function equalPowerGain() {
  return Math.SQRT1_2;
}

function checkDistanceResult(renderedBuffer, model, should) {
  renderedData = renderedBuffer.getChannelData(0);

  // The max allowed error between the actual gain and the expected
  // value.  This is determined experimentally.  Set to 0 to see
  // what the actual errors are.
  let maxAllowedError = 2.2720e-6;

  let success = true;

  // Number of impulses we found in the rendered result.
  let impulseCount = 0;

  // Maximum relative error in the gain of the impulses.
  let maxError = 0;

  // Array of locations of the impulses that were not at the
  // expected location.  (Contains the actual and expected frame
  // of the impulse.)
  let impulsePositionErrors = new Array();

  // Step through the rendered data to find all the non-zero points
  // so we can find where our distance-attenuated impulses are.
  // These are tested against the expected attenuations at that
  // distance.
  for (let k = 0; k < renderedData.length; ++k) {
    if (renderedData[k] != 0) {
      // Convert from string to index.
      let distanceFunction = distanceModelFunction[model];
      let expected =
          distanceFunction(panner[impulseCount], 0, 0, position[impulseCount]);

      // Adjust for the center-panning of the EQUALPOWER panning
      // model that we're using.
      expected *= equalPowerGain();

      let error = Math.abs(renderedData[k] - expected) / Math.abs(expected);

      maxError = Math.max(maxError, Math.abs(error));

      should(renderedData[k]).beCloseTo(expected, {threshold: maxAllowedError});

      // Keep track of any impulses that aren't where we expect them
      // to be.
      let expectedOffset = timeToSampleFrame(time[impulseCount], sampleRate);
      if (k != expectedOffset) {
        impulsePositionErrors.push({actual: k, expected: expectedOffset});
      }
      ++impulseCount;
    }
  }
  should(impulseCount, 'Number of impulses').beEqualTo(nodesToCreate);

  should(maxError, 'Max error in distance gains')
      .beLessThanOrEqualTo(maxAllowedError);

  // Display any timing errors that we found.
  if (impulsePositionErrors.length > 0) {
    let actual = impulsePositionErrors.map(x => x.actual);
    let expected = impulsePositionErrors.map(x => x.expected);
    should(actual, 'Actual impulse positions found').beEqualToArray(expected);
  }
}
