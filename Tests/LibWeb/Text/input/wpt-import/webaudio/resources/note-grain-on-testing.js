// Use a power of two to eliminate round-off converting from frames to time.
let sampleRate = 32768;

// How many grains to play.
let numberOfTests = 100;

// Duration of each grain to be played.  Make a whole number of frames
let duration = Math.floor(0.01 * sampleRate) / sampleRate;

// A little extra bit of silence between grain boundaries.  Must be a whole
// number of frames.
let grainGap = Math.floor(0.005 * sampleRate) / sampleRate;

// Time step between the start of each grain.  We need to add a little
// bit of silence so we can detect grain boundaries
let timeStep = duration + grainGap;

// Time step between the start for each grain.  Must be a whole number of
// frames.
let grainOffsetStep = Math.floor(0.001 * sampleRate) / sampleRate;

// How long to render to cover all of the grains.
let renderTime = (numberOfTests + 1) * timeStep;

let context;
let renderedData;

// Create a buffer containing the data that we want.  The function f
// returns the desired value at sample frame k.
function createSignalBuffer(context, f) {
  // Make sure the buffer has enough data for all of the possible
  // grain offsets and durations.  The additional 1 is for any
  // round-off errors.
  let signalLength =
      Math.floor(1 + sampleRate * (numberOfTests * grainOffsetStep + duration));

  let buffer = context.createBuffer(2, signalLength, sampleRate);
  let data = buffer.getChannelData(0);

  for (let k = 0; k < signalLength; ++k) {
    data[k] = f(k);
  }

  return buffer;
}

// From the data array, find the start and end sample frame for each
// grain.  This depends on the data having 0's between grain, and
// that the grain is always strictly non-zero.
function findStartAndEndSamples(data) {
  let nSamples = data.length;

  let startTime = [];
  let endTime = [];
  let lookForStart = true;

  // Look through the rendered data to find the start and stop
  // times of each grain.
  for (let k = 0; k < nSamples; ++k) {
    if (lookForStart) {
      // Find a non-zero point and record the start.  We're not
      // concerned with the value in this test, only that the
      // grain started here.
      if (renderedData[k]) {
        startTime.push(k);
        lookForStart = false;
      }
    } else {
      // Find a zero and record the end of the grain.
      if (!renderedData[k]) {
        endTime.push(k);
        lookForStart = true;
      }
    }
  }

  return {start: startTime, end: endTime};
}

function playGrain(context, source, time, offset, duration) {
  let bufferSource = context.createBufferSource();

  bufferSource.buffer = source;
  bufferSource.connect(context.destination);
  bufferSource.start(time, offset, duration);
}

// Play out all grains.  Returns a object containing two arrays, one
// for the start time and one for the grain offset time.
function playAllGrains(context, source, numberOfNotes) {
  let startTimes = new Array(numberOfNotes);
  let offsets = new Array(numberOfNotes);

  for (let k = 0; k < numberOfNotes; ++k) {
    let timeOffset = k * timeStep;
    let grainOffset = k * grainOffsetStep;

    playGrain(context, source, timeOffset, grainOffset, duration);
    startTimes[k] = timeOffset;
    offsets[k] = grainOffset;
  }

  return {startTimes: startTimes, grainOffsetTimes: offsets};
}

// Verify that the start and end frames for each grain match our
// expected start and end frames.
function verifyStartAndEndFrames(startEndFrames, should) {
  let startFrames = startEndFrames.start;
  let endFrames = startEndFrames.end;

  // Count of how many grains started at the incorrect time.
  let errorCountStart = 0;

  // Count of how many grains ended at the incorrect time.
  let errorCountEnd = 0;

  should(
      startFrames.length == endFrames.length, 'Found all grain starts and ends')
      .beTrue();

  should(startFrames.length, 'Number of start frames').beEqualTo(numberOfTests);
  should(endFrames.length, 'Number of end frames').beEqualTo(numberOfTests);

  // Examine the start and stop times to see if they match our
  // expectations.
  for (let k = 0; k < startFrames.length; ++k) {
    let expectedStart = timeToSampleFrame(k * timeStep, sampleRate);
    // The end point is the duration.
    let expectedEnd = expectedStart +
        grainLengthInSampleFrames(k * grainOffsetStep, duration, sampleRate);

    if (startFrames[k] != expectedStart) {
      ++errorCountStart;
    }
    if (endFrames[k] != expectedEnd) {
      ++errorCountEnd;
    }

    should([startFrames[k], endFrames[k]], 'Pulse ' + k + ' boundary')
        .beEqualToArray([expectedStart, expectedEnd]);
  }

  // Check that all the grains started or ended at the correct time.
  if (!errorCountStart) {
    should(
        startFrames.length, 'Number of grains that started at the correct time')
        .beEqualTo(numberOfTests);
  } else {
    should(
        errorCountStart,
        'Number of grains out of ' + numberOfTests +
            'that started at the wrong time')
        .beEqualTo(0);
  }

  if (!errorCountEnd) {
    should(endFrames.length, 'Number of grains that ended at the correct time')
        .beEqualTo(numberOfTests);
  } else {
    should(
        errorCountEnd,
        'Number of grains out of ' + numberOfTests +
            ' that ended at the wrong time')
        .beEqualTo(0);
  }
}

function verifyStartAndEndFrames_W3CTH(startEndFrames) {
  const startFrames = startEndFrames.start;
  const endFrames = startEndFrames.end;

  let errorCountStart = 0;
  let errorCountEnd = 0;

  assert_equals(
      startFrames.length, endFrames.length,
      'Found all grain starts and ends');
  assert_equals(startFrames.length, numberOfTests, 'Number of start frames');
  assert_equals(endFrames.length, numberOfTests, 'Number of end frames');

  for (let k = 0; k < startFrames.length; ++k) {
    const expectedStart = timeToSampleFrame(k * timeStep, sampleRate);
    const expectedEnd =
        expectedStart + grainLengthInSampleFrames(
            k * grainOffsetStep, duration, sampleRate);

    if (startFrames[k] !== expectedStart) {
      ++errorCountStart;
    }
    if (endFrames[k] !== expectedEnd) {
      ++errorCountEnd;
    }

    assert_array_equals(
        [startFrames[k], endFrames[k]],
        [expectedStart, expectedEnd],
        'Pulse ' + k + ' boundary');
  }

  if (!errorCountStart) {
    assert_equals(
        startFrames.length, numberOfTests,
        'Number of grains that started at the correct time');
  } else {
    assert_equals(
        errorCountStart, 0,
        `Number of grains out of ${numberOfTests} that ` +
            `started at the wrong time`);
  }

  if (!errorCountEnd) {
    assert_equals(
        endFrames.length, numberOfTests,
        'Number of grains that ended at the correct time');
  } else {
    assert_equals(
        errorCountEnd, 0,
        `Number of grains out of ${numberOfTests} that ` +
            `ended at the wrong time`);
  }
}
