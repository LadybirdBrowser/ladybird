// Use a power of two to eliminate round-off when converting frames to time and
// vice versa.
let sampleRate = 32768;

let numberOfChannels = 1;

// Time step when each panner node starts.  Make sure it starts on a frame
// boundary.
let timeStep = Math.floor(0.001 * sampleRate) / sampleRate;

// Length of the impulse signal.
let pulseLengthFrames = Math.round(timeStep * sampleRate);

// How many panner nodes to create for the test
let nodesToCreate = 100;

// Be sure we render long enough for all of our nodes.
let renderLengthSeconds = timeStep * (nodesToCreate + 1);

// These are global mostly for debugging.
let context;
let impulse;
let bufferSource;
let panner;
let position;
let time;

let renderedBuffer;
let renderedLeft;
let renderedRight;

function createGraph(context, nodeCount, positionSetter) {
  bufferSource = new Array(nodeCount);
  panner = new Array(nodeCount);
  position = new Array(nodeCount);
  time = new Array(nodeCount);
  // Angle between panner locations.  (nodeCount - 1 because we want
  // to include both 0 and 180 deg.
  let angleStep = Math.PI / (nodeCount - 1);

  if (numberOfChannels == 2) {
    impulse = createStereoImpulseBuffer(context, pulseLengthFrames);
  } else
    impulse = createImpulseBuffer(context, pulseLengthFrames);

  for (let k = 0; k < nodeCount; ++k) {
    bufferSource[k] = context.createBufferSource();
    bufferSource[k].buffer = impulse;

    panner[k] = context.createPanner();
    panner[k].panningModel = 'equalpower';
    panner[k].distanceModel = 'linear';

    let angle = angleStep * k;
    position[k] = {angle: angle, x: Math.cos(angle), z: Math.sin(angle)};
    positionSetter(panner[k], position[k].x, 0, position[k].z);

    bufferSource[k].connect(panner[k]);
    panner[k].connect(context.destination);

    // Start the source
    time[k] = k * timeStep;
    bufferSource[k].start(time[k]);
  }
}

function createTestAndRun(
    context, should, nodeCount, numberOfSourceChannels, positionSetter) {
  numberOfChannels = numberOfSourceChannels;

  createGraph(context, nodeCount, positionSetter);

  return context.startRendering().then(buffer => checkResult(buffer, should));
}

async function createTestAndRun_W3CTH(
    context, nodeCount, numberOfSourceChannels, positionSetter) {
  numberOfChannels = numberOfSourceChannels;
  createGraph(context, nodeCount, positionSetter);
  const renderedBuffer = await context.startRendering();
  return checkResult_W3CTH(renderedBuffer);
}

// Map our position angle to the azimuth angle (in degrees).
//
// An angle of 0 corresponds to an azimuth of 90 deg; pi, to -90 deg.
function angleToAzimuth(angle) {
  return 90 - angle * 180 / Math.PI;
}

// The gain caused by the EQUALPOWER panning model
function equalPowerGain(angle) {
  let azimuth = angleToAzimuth(angle);

  if (numberOfChannels == 1) {
    let panPosition = (azimuth + 90) / 180;

    let gainL = Math.cos(0.5 * Math.PI * panPosition);
    let gainR = Math.sin(0.5 * Math.PI * panPosition);

    return {left: gainL, right: gainR};
  } else {
    if (azimuth <= 0) {
      let panPosition = (azimuth + 90) / 90;

      let gainL = 1 + Math.cos(0.5 * Math.PI * panPosition);
      let gainR = Math.sin(0.5 * Math.PI * panPosition);

      return {left: gainL, right: gainR};
    } else {
      let panPosition = azimuth / 90;

      let gainL = Math.cos(0.5 * Math.PI * panPosition);
      let gainR = 1 + Math.sin(0.5 * Math.PI * panPosition);

      return {left: gainL, right: gainR};
    }
  }
}

function checkResult(renderedBuffer, should) {
  renderedLeft = renderedBuffer.getChannelData(0);
  renderedRight = renderedBuffer.getChannelData(1);

  // The max error we allow between the rendered impulse and the
  // expected value.  This value is experimentally determined.  Set
  // to 0 to make the test fail to see what the actual error is.
  let maxAllowedError = 1.1597e-6;

  let success = true;

  // Number of impulses found in the rendered result.
  let impulseCount = 0;

  // Max (relative) error and the index of the maxima for the left
  // and right channels.
  let maxErrorL = 0;
  let maxErrorIndexL = 0;
  let maxErrorR = 0;
  let maxErrorIndexR = 0;

  // Number of impulses that don't match our expected locations.
  let timeCount = 0;

  // Locations of where the impulses aren't at the expected locations.
  let timeErrors = new Array();

  for (let k = 0; k < renderedLeft.length; ++k) {
    // We assume that the left and right channels start at the same instant.
    if (renderedLeft[k] != 0 || renderedRight[k] != 0) {
      // The expected gain for the left and right channels.
      let pannerGain = equalPowerGain(position[impulseCount].angle);
      let expectedL = pannerGain.left;
      let expectedR = pannerGain.right;

      // Absolute error in the gain.
      let errorL = Math.abs(renderedLeft[k] - expectedL);
      let errorR = Math.abs(renderedRight[k] - expectedR);

      if (Math.abs(errorL) > maxErrorL) {
        maxErrorL = Math.abs(errorL);
        maxErrorIndexL = impulseCount;
      }
      if (Math.abs(errorR) > maxErrorR) {
        maxErrorR = Math.abs(errorR);
        maxErrorIndexR = impulseCount;
      }

      // Keep track of the impulses that didn't show up where we
      // expected them to be.
      let expectedOffset = timeToSampleFrame(time[impulseCount], sampleRate);
      if (k != expectedOffset) {
        timeErrors[timeCount] = {actual: k, expected: expectedOffset};
        ++timeCount;
      }
      ++impulseCount;
    }
  }

  should(impulseCount, 'Number of impulses found').beEqualTo(nodesToCreate);

  should(
      timeErrors.map(x => x.actual),
      'Offsets of impulses at the wrong position')
      .beEqualToArray(timeErrors.map(x => x.expected));

  should(maxErrorL, 'Error in left channel gain values')
      .beLessThanOrEqualTo(maxAllowedError);

  should(maxErrorR, 'Error in right channel gain values')
      .beLessThanOrEqualTo(maxAllowedError);
}

function checkResult_W3CTH(renderedBuffer) {
  // The max error we allow between the rendered impulse and the
  // expected value.  This value is experimentally determined.  Set
  // to 0 to make the test fail to see what the actual error is.
  renderedLeft = renderedBuffer.getChannelData(0);
  renderedRight = renderedBuffer.getChannelData(1);

  const maxAllowedError = 1.1597e-6;
  let impulseCount = 0;
  let maxErrorL = 0;
  let maxErrorR = 0;
  const timeErrors = [];

  for (let k = 0; k < renderedLeft.length; ++k) {
    if (renderedLeft[k] !== 0 || renderedRight[k] !== 0) {
      const {left: expectedL, right: expectedR} =
          equalPowerGain(position[impulseCount].angle);

      maxErrorL = Math.max(maxErrorL, Math.abs(renderedLeft[k] - expectedL));
      maxErrorR = Math.max(maxErrorR, Math.abs(renderedRight[k] - expectedR));

      const expectedOffset =
          timeToSampleFrame(time[impulseCount], sampleRate);
      if (k !== expectedOffset) {
        timeErrors.push({actual: k, expected: expectedOffset});
      }

      ++impulseCount;
    }
  }

  assert_equals(
      impulseCount,
      nodesToCreate,
      'Number of impulses found');

  assert_array_equals(
      timeErrors.map(e => e.actual),
      timeErrors.map(e => e.expected),
      'Offsets of impulses at the wrong position');

  assert_less_than_equal(maxErrorL, maxAllowedError,
      `Left-channel gain error ${maxErrorL} > ${maxAllowedError}`);
  assert_less_than_equal(maxErrorR, maxAllowedError,
      `Right-channel gain error ${maxErrorR} > ${maxAllowedError}`);
}