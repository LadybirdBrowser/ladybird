(function(global) {

  // Information about the starting/ending times and starting/ending values for
  // each time interval.
  let timeValueInfo;

  // The difference between starting values between each time interval.
  let startingValueDelta;

  // For any automation function that has an end or target value, the end value
  // is based the starting value of the time interval.  The starting value will
  // be increased or decreased by |startEndValueChange|. We choose half of
  // |startingValueDelta| so that the ending value will be distinct from the
  // starting value for next time interval.  This allows us to detect where the
  // ramp begins and ends.
  let startEndValueChange;

  // Default threshold to use for detecting discontinuities that should appear
  // at each time interval.
  let discontinuityThreshold;

  // Time interval between value changes.  It is best if 1 / numberOfTests is
  // not close to timeInterval.
  let timeIntervalInternal = .03;

  let context;

  // Make sure we render long enough to capture all of our test data.
  function renderLength(numberOfTests) {
    return timeToSampleFrame((numberOfTests + 1) * timeInterval, sampleRate);
  }

  // Create a constant reference signal with the given |value|.  Basically the
  // same as |createConstantBuffer|, but with the parameters to match the other
  // create functions.  The |endValue| is ignored.
  function createConstantArray(
      startTime, endTime, value, endValue, sampleRate) {
    let startFrame = timeToSampleFrame(startTime, sampleRate);
    let endFrame = timeToSampleFrame(endTime, sampleRate);
    let length = endFrame - startFrame;

    let buffer = createConstantBuffer(context, length, value);

    return buffer.getChannelData(0);
  }

  function getStartEndFrames(startTime, endTime, sampleRate) {
    // Start frame is the ceiling of the start time because the ramp starts at
    // or after the sample frame.  End frame is the ceiling because it's the
    // exclusive ending frame of the automation.
    let startFrame = Math.ceil(startTime * sampleRate);
    let endFrame = Math.ceil(endTime * sampleRate);

    return {startFrame: startFrame, endFrame: endFrame};
  }

  // Create a linear ramp starting at |startValue| and ending at |endValue|. The
  // ramp starts at time |startTime| and ends at |endTime|.  (The start and end
  // times are only used to compute how many samples to return.)
  function createLinearRampArray(
      startTime, endTime, startValue, endValue, sampleRate) {
    let frameInfo = getStartEndFrames(startTime, endTime, sampleRate);
    let startFrame = frameInfo.startFrame;
    let endFrame = frameInfo.endFrame;
    let length = endFrame - startFrame;
    let array = new Array(length);

    let step = Math.fround(
        (endValue - startValue) / (endTime - startTime) / sampleRate);
    let start = Math.fround(
        startValue +
        (endValue - startValue) * (startFrame / sampleRate - startTime) /
            (endTime - startTime));

    let slope = (endValue - startValue) / (endTime - startTime);

    // v(t) = v0 + (v1 - v0)*(t-t0)/(t1-t0)
    for (k = 0; k < length; ++k) {
      // array[k] = Math.fround(start + k * step);
      let t = (startFrame + k) / sampleRate;
      array[k] = startValue + slope * (t - startTime);
    }

    return array;
  }

  // Create an exponential ramp starting at |startValue| and ending at
  // |endValue|. The ramp starts at time |startTime| and ends at |endTime|.
  // (The start and end times are only used to compute how many samples to
  // return.)
  function createExponentialRampArray(
      startTime, endTime, startValue, endValue, sampleRate) {
    let deltaTime = endTime - startTime;

    let frameInfo = getStartEndFrames(startTime, endTime, sampleRate);
    let startFrame = frameInfo.startFrame;
    let endFrame = frameInfo.endFrame;
    let length = endFrame - startFrame;
    let array = new Array(length);

    let ratio = endValue / startValue;

    // v(t) = v0*(v1/v0)^((t-t0)/(t1-t0))
    for (let k = 0; k < length; ++k) {
      let t = Math.fround((startFrame + k) / sampleRate);
      array[k] = Math.fround(
          startValue * Math.pow(ratio, (t - startTime) / deltaTime));
    }

    return array;
  }

  function discreteTimeConstantForSampleRate(timeConstant, sampleRate) {
    return 1 - Math.exp(-1 / (sampleRate * timeConstant));
  }

  // Create a signal that starts at |startValue| and exponentially approaches
  // the target value of |targetValue|, using a time constant of |timeConstant|.
  // The ramp starts at time |startTime| and ends at |endTime|.  (The start and
  // end times are only used to compute how many samples to return.)
  function createExponentialApproachArray(
      startTime, endTime, startValue, targetValue, sampleRate, timeConstant) {
    let startFrameFloat = startTime * sampleRate;
    let frameInfo = getStartEndFrames(startTime, endTime, sampleRate);
    let startFrame = frameInfo.startFrame;
    let endFrame = frameInfo.endFrame;
    let length = Math.floor(endFrame - startFrame);
    let array = new Array(length);
    let c = discreteTimeConstantForSampleRate(timeConstant, sampleRate);

    let delta = startValue - targetValue;

    // v(t) = v1 + (v0 - v1) * exp(-(t-t0)/tau)
    for (let k = 0; k < length; ++k) {
      let t = (startFrame + k) / sampleRate;
      let value =
          targetValue + delta * Math.exp(-(t - startTime) / timeConstant);
      array[k] = value;
    }

    return array;
  }

  // Create a sine wave of the specified duration.
  function createReferenceSineArray(
      startTime, endTime, startValue, endValue, sampleRate) {
    // Ignore |startValue| and |endValue| for the sine wave.
    let curve = createSineWaveArray(
        endTime - startTime, freqHz, sineAmplitude, sampleRate);
    // Sample the curve appropriately.
    let frameInfo = getStartEndFrames(startTime, endTime, sampleRate);
    let startFrame = frameInfo.startFrame;
    let endFrame = frameInfo.endFrame;
    let length = Math.floor(endFrame - startFrame);
    let array = new Array(length);

    // v(t) = linearly interpolate between V[k] and V[k + 1] where k =
    // floor((N-1)/duration*(t - t0))
    let f = (length - 1) / (endTime - startTime);

    for (let k = 0; k < length; ++k) {
      let t = (startFrame + k) / sampleRate;
      let indexFloat = f * (t - startTime);
      let index = Math.floor(indexFloat);
      if (index + 1 < length) {
        let v0 = curve[index];
        let v1 = curve[index + 1];
        array[k] = v0 + (v1 - v0) * (indexFloat - index);
      } else {
        array[k] = curve[length - 1];
      }
    }

    return array;
  }

  // Create a sine wave of the given frequency and amplitude.  The sine wave is
  // offset by half the amplitude so that result is always positive.
  function createSineWaveArray(durationSeconds, freqHz, amplitude, sampleRate) {
    let length = timeToSampleFrame(durationSeconds, sampleRate);
    let signal = new Float32Array(length);
    let omega = 2 * Math.PI * freqHz / sampleRate;
    let halfAmplitude = amplitude / 2;

    for (let k = 0; k < length; ++k) {
      signal[k] = halfAmplitude + halfAmplitude * Math.sin(omega * k);
    }

    return signal;
  }

  // Return the difference between the starting value and the ending value for
  // time interval |timeIntervalIndex|.  We alternate between an end value that
  // is above or below the starting value.
  function endValueDelta(timeIntervalIndex) {
    if (timeIntervalIndex & 1) {
      return -startEndValueChange;
    } else {
      return startEndValueChange;
    }
  }

  // Relative error metric
  function relativeErrorMetric(actual, expected) {
    return (actual - expected) / Math.abs(expected);
  }

  // Difference metric
  function differenceErrorMetric(actual, expected) {
    return actual - expected;
  }

  // Return the difference between the starting value at |timeIntervalIndex| and
  // the starting value at the next time interval.  Since we started at a large
  // initial value, we decrease the value at each time interval.
  function valueUpdate(timeIntervalIndex) {
    return -startingValueDelta;
  }

  // Compare a section of the rendered data against our expected signal.
  function comparePartialSignals(
      rendered, expectedFunction, startTime, endTime, valueInfo,
      sampleRateParam, errorMetric) {
    let startSample = timeToSampleFrame(startTime, sampleRateParam);
    let expected = expectedFunction(
        startTime, endTime, valueInfo.startValue, valueInfo.endValue,
        sampleRateParam, timeConstant);

    let n = expected.length;
    let maxError = -1;
    let maxErrorIndex = -1;

    for (let k = 0; k < n; ++k) {
      // Make sure we don't pass these tests because a NaN has been generated in
      // either the
      // rendered data or the reference data.
      if (!isValidNumber(rendered[startSample + k])) {
        maxError = Infinity;
        maxErrorIndex = startSample + k;
        assert_true(
            isValidNumber(rendered[startSample + k]),
            `NaN or infinity for rendered data at ${maxErrorIndex}`);
        break;
      }
      if (!isValidNumber(expected[k])) {
        maxError = Infinity;
        maxErrorIndex = startSample + k;
        assert_true(
            isValidNumber(expected[k]),
            `NaN or infinity for rendered data at ${maxErrorIndex}`);
        break;
      }
      let error = Math.abs(errorMetric(rendered[startSample + k], expected[k]));
      if (error > maxError) {
        maxError = error;
        maxErrorIndex = k;
      }
    }

    return {maxError: maxError, index: maxErrorIndex, expected: expected};
  };

  // Find the discontinuities in the data and compare the locations of the
  // discontinuities with the times that define the time intervals. There is a
  // discontinuity if the difference between successive samples exceeds the
  // threshold.
  function verifyDiscontinuities(values, times, threshold) {
    let n = values.length;
    let success = true;
    let badLocations = 0;
    let breaks = [];

    // Find discontinuities.
    for (let k = 1; k < n; ++k) {
      if (Math.abs(values[k] - values[k - 1]) > threshold) {
        breaks.push(k);
      }
    }

    let testCount;

    // If there are numberOfTests intervals, there are only numberOfTests - 1
    // internal interval boundaries. Hence the maximum number of discontinuties
    // we expect to find is numberOfTests - 1. If we find more than that, we
    // have no reference to compare against. We also assume that the actual
    // discontinuities are close to the expected ones.
    //
    // This is just a sanity check when something goes really wrong.  For
    // example, if the threshold is too low, every sample frame looks like a
    // discontinuity.
    if (breaks.length >= numberOfTests) {
      testCount = numberOfTests - 1;
      assert_less_than(
          breaks.length, numberOfTests, 'Number of discontinuities');
      success = false;
    } else {
      testCount = breaks.length;
    }

    // Compare the location of each discontinuity with the end time of each
    // interval. (There is no discontinuity at the start of the signal.)
    for (let k = 0; k < testCount; ++k) {
      let expectedSampleFrame = timeToSampleFrame(times[k + 1], sampleRate);
      if (breaks[k] != expectedSampleFrame) {
        success = false;
        ++badLocations;
        assert_equals(breaks[k], expectedSampleFrame, 'Discontinuity at index');
      }
    }

    if (badLocations) {
      assert_equals(
          badLocations, 0, 'Number of discontinuites at incorrect locations');
      success = false;
    } else {
      assert_equals(
          breaks.length + 1,
          numberOfTests,
          'Number of tests started and ended at the correct time');
    }

    return success;
  };

  // Compare the rendered data with the expected data.
  //
  // testName - string describing the test
  //
  // maxError - maximum allowed difference between the rendered data and the
  // expected data
  //
  // rendererdData - array containing the rendered (actual) data
  //
  // expectedFunction - function to compute the expected data
  //
  // timeValueInfo - array containing information about the start and end times
  // and the start and end values of each interval.
  //
  // breakThreshold - threshold to use for determining discontinuities.
  function compareSignals(
      testName, maxError, renderedData, expectedFunction, timeValueInfo,
      breakThreshold, errorMetric) {
    let success = true;
    const failedTestCount = 0;
    const times = timeValueInfo.times;
    const values = timeValueInfo.values;
    const n = values.length;
    let expectedSignal = [];

    success =
        verifyDiscontinuities(renderedData, times, breakThreshold);

    for (let k = 0; k < n; ++k) {
      const result = comparePartialSignals(
          renderedData, expectedFunction, times[k], times[k + 1],
          values[k], sampleRate, errorMetric);

      expectedSignal =
          expectedSignal.concat(Array.prototype.slice.call(result.expected));

      assert_less_than_equal(
          result.maxError,
          maxError,
          `Max error for test ${k} at offset ` +
              `${result.index + timeToSampleFrame(times[k], sampleRate)}`);
    }

    assert_equals(
        failedTestCount,
        0,
        `Number of failed tests with an acceptable relative ` +
            `tolerance of ${maxError}`);
  };

  // Create a function to test the rendered data with the reference data.
  //
  // testName - string describing the test
  //
  // error - max allowed error between rendered data and the reference data.
  //
  // referenceFunction - function that generates the reference data to be
  // compared with the rendered data.
  //
  // jumpThreshold - optional parameter that specifies the threshold to use for
  // detecting discontinuities.  If not specified, defaults to
  // discontinuityThreshold.
  //
  function checkResultFunction(
      task, testName, error, referenceFunction, jumpThreshold,
      errorMetric) {
    return function(event) {
      let buffer = event.renderedBuffer;
      renderedData = buffer.getChannelData(0);

      let threshold;

      if (!jumpThreshold) {
        threshold = discontinuityThreshold;
      } else {
        threshold = jumpThreshold;
      }

      compareSignals(
          testName, error, renderedData, referenceFunction,
          timeValueInfo, threshold, errorMetric);
      task.done();
    }
  }

  // Run all the automation tests.
  //
  // numberOfTests - number of tests (time intervals) to run.
  //
  // initialValue - The initial value of the first time interval.
  //
  // setValueFunction - function that sets the specified value at the start of a
  // time interval.
  //
  // automationFunction - function that sets the end value for the time
  // interval. It specifies how the value approaches the end value.
  //
  // An object is returned containing an array of start times for each time
  // interval, and an array giving the start and end values for the interval.
  function doAutomation(
      numberOfTests, initialValue, setValueFunction, automationFunction) {
    let timeInfo = [0];
    let valueInfo = [];
    let value = initialValue;

    for (let k = 0; k < numberOfTests; ++k) {
      let startTime = k * timeInterval;
      let endTime = (k + 1) * timeInterval;
      let endValue = value + endValueDelta(k);

      // Set the value at the start of the time interval.
      setValueFunction(value, startTime);

      // Specify the end or target value, and how we should approach it.
      automationFunction(endValue, startTime, endTime);

      // Keep track of the start times, and the start and end values for each
      // time interval.
      timeInfo.push(endTime);
      valueInfo.push({startValue: value, endValue: endValue});

      value += valueUpdate(k);
    }

    return {times: timeInfo, values: valueInfo};
  }

  // Create the audio graph for the test and then run the test.
  //
  // numberOfTests - number of time intervals (tests) to run.
  //
  // initialValue - the initial value of the gain at time 0.
  //
  // setValueFunction - function to set the value at the beginning of each time
  // interval.
  //
  // automationFunction - the AudioParamTimeline automation function
  //
  // testName - string indicating the test that is being run.
  //
  // maxError - maximum allowed error between the rendered data and the
  // reference data
  //
  // referenceFunction - function that generates the reference data to be
  // compared against the rendered data.
  //
  // jumpThreshold - optional parameter that specifies the threshold to use for
  // detecting discontinuities.  If not specified, defaults to
  // discontinuityThreshold.
  function createAudioGraphAndTest(
      task, should, numberOfTests, initialValue, setValueFunction,
      automationFunction, testName, maxError, referenceFunction, jumpThreshold,
      errorMetric) {
    // Create offline audio context.
    context =
        new OfflineAudioContext(2, renderLength(numberOfTests), sampleRate);
    let constantBuffer =
        createConstantBuffer(context, renderLength(numberOfTests), 1);

    // We use an AudioGainNode here simply as a convenient way to test the
    // AudioParam automation, since it's easy to pass a constant value through
    // the node, automate the .gain attribute and observe the resulting values.

    gainNode = context.createGain();

    let bufferSource = context.createBufferSource();
    bufferSource.buffer = constantBuffer;
    bufferSource.connect(gainNode);
    gainNode.connect(context.destination);

    // Set up default values for the parameters that control how the automation
    // test values progress for each time interval.
    startingValueDelta = initialValue / numberOfTests;
    startEndValueChange = startingValueDelta / 2;
    discontinuityThreshold = startEndValueChange / 2;

    // Run the automation tests.
    timeValueInfo = doAutomation(
        numberOfTests, initialValue, setValueFunction, automationFunction);
    bufferSource.start(0);

    context.oncomplete = checkResultFunction(
        task, testName, maxError, referenceFunction, jumpThreshold,
        errorMetric || relativeErrorMetric);
    context.startRendering();
  }

  // Export local references to global scope. All the new objects in this file
  // must be exported through this if it is to be used in the actual test HTML
  // page.
  let exports = {
    'sampleRate': 44100,
    'gainNode': null,
    'timeInterval': timeIntervalInternal,

    // Some suitable time constant so that we can see a significant change over
    // a timeInterval.  This is only needed by setTargetAtTime() which needs a
    // time constant.
    'timeConstant': timeIntervalInternal / 3,

    'renderLength': renderLength,
    'createConstantArray': createConstantArray,
    'getStartEndFrames': getStartEndFrames,
    'createLinearRampArray': createLinearRampArray,
    'createExponentialRampArray': createExponentialRampArray,
    'discreteTimeConstantForSampleRate': discreteTimeConstantForSampleRate,
    'createExponentialApproachArray': createExponentialApproachArray,
    'createReferenceSineArray': createReferenceSineArray,
    'createSineWaveArray': createSineWaveArray,
    'endValueDelta': endValueDelta,
    'relativeErrorMetric': relativeErrorMetric,
    'differenceErrorMetric': differenceErrorMetric,
    'valueUpdate': valueUpdate,
    'comparePartialSignals': comparePartialSignals,
    'verifyDiscontinuities': verifyDiscontinuities,
    'compareSignals': compareSignals,
    'checkResultFunction': checkResultFunction,
    'doAutomation': doAutomation,
    'createAudioGraphAndTest': createAudioGraphAndTest
  };

  for (let reference in exports) {
    global[reference] = exports[reference];
  }

})(window);
