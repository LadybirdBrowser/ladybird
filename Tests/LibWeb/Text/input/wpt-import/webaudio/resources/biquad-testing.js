// Globals, to make testing and debugging easier.
let context;
let filter;
let signal;
let renderedBuffer;
let renderedData;

// Use a power of two to eliminate round-off in converting frame to time
let sampleRate = 32768;
let pulseLengthFrames = .1 * sampleRate;

// Maximum allowed error for the test to succeed.  Experimentally determined.
let maxAllowedError = 5.9e-8;

// This must be large enough so that the filtered result is essentially zero.
// See comments for createTestAndRun.  This must be a whole number of frames.
let timeStep = Math.ceil(.1 * sampleRate) / sampleRate;

// Maximum number of filters we can process (mostly for setting the
// render length correctly.)
let maxFilters = 5;

// How long to render.  Must be long enough for all of the filters we
// want to test.
let renderLengthSeconds = timeStep * (maxFilters + 1);

let renderLengthSamples = Math.round(renderLengthSeconds * sampleRate);

// Number of filters that will be processed.
let nFilters;

function createImpulseBuffer(context, length) {
  let impulse = context.createBuffer(1, length, context.sampleRate);
  let data = impulse.getChannelData(0);
  for (let k = 1; k < data.length; ++k) {
    data[k] = 0;
  }
  data[0] = 1;

  return impulse;
}


function createTestAndRun(context, filterType, testParameters) {
  // To test the filters, we apply a signal (an impulse) to each of
  // the specified filters, with each signal starting at a different
  // time.  The output of the filters is summed together at the
  // output.  Thus for filter k, the signal input to the filter
  // starts at time k * timeStep.  For this to work well, timeStep
  // must be large enough for the output of each filter to have
  // decayed to zero with timeStep seconds.  That way the filter
  // outputs don't interfere with each other.

  let filterParameters = testParameters.filterParameters;
  nFilters = Math.min(filterParameters.length, maxFilters);

  signal = new Array(nFilters);
  filter = new Array(nFilters);

  impulse = createImpulseBuffer(context, pulseLengthFrames);

  // Create all of the signal sources and filters that we need.
  for (let k = 0; k < nFilters; ++k) {
    signal[k] = context.createBufferSource();
    signal[k].buffer = impulse;

    filter[k] = context.createBiquadFilter();
    filter[k].type = filterType;
    filter[k].frequency.value =
        context.sampleRate / 2 * filterParameters[k].cutoff;
    filter[k].detune.value = (filterParameters[k].detune === undefined) ?
        0 :
        filterParameters[k].detune;
    filter[k].Q.value = filterParameters[k].q;
    filter[k].gain.value = filterParameters[k].gain;

    signal[k].connect(filter[k]);
    filter[k].connect(context.destination);

    signal[k].start(timeStep * k);
  }

  return context.startRendering().then(buffer => {
    checkFilterResponse(buffer, filterType, testParameters);
  });
}

function addSignal(dest, src, destOffset) {
  // Add src to dest at the given dest offset.
  for (let k = destOffset, j = 0; k < dest.length, j < src.length; ++k, ++j) {
    dest[k] += src[j];
  }
}

function generateReference(filterType, filterParameters) {
  let result = new Array(renderLengthSamples);
  let data = new Array(renderLengthSamples);
  // Initialize the result array and data.
  for (let k = 0; k < result.length; ++k) {
    result[k] = 0;
    data[k] = 0;
  }
  // Make data an impulse.
  data[0] = 1;

  for (let k = 0; k < nFilters; ++k) {
    // Filter an impulse
    let detune = (filterParameters[k].detune === undefined) ?
        0 :
        filterParameters[k].detune;
    let frequency = filterParameters[k].cutoff *
        Math.pow(2, detune / 1200);  // Apply detune, converting from Cents.

    let filterCoef = createFilter(
        filterType, frequency, filterParameters[k].q, filterParameters[k].gain);
    let y = filterData(filterCoef, data, renderLengthSamples);

    // Accumulate this filtered data into the final output at the desired
    // offset.
    addSignal(result, y, timeToSampleFrame(timeStep * k, sampleRate));
  }

  return result;
}

function checkFilterResponse(renderedBuffer, filterType, testParameters) {
  let filterParameters = testParameters.filterParameters;
  let maxAllowedError = testParameters.threshold;
  let should = testParameters.should;

  renderedData = renderedBuffer.getChannelData(0);

  reference = generateReference(filterType, filterParameters);

  let len = Math.min(renderedData.length, reference.length);

  let success = true;

  // Maximum error between rendered data and expected data
  let maxError = 0;

  // Sample offset where the maximum error occurred.
  let maxPosition = 0;

  // Number of infinities or NaNs that occurred in the rendered data.
  let invalidNumberCount = 0;

  should(nFilters, 'Number of filters tested')
      .beEqualTo(filterParameters.length);

  // Compare the rendered signal with our reference, keeping
  // track of the maximum difference (and the offset of the max
  // difference.)  Check for bad numbers in the rendered output
  // too.  There shouldn't be any.
  for (let k = 0; k < len; ++k) {
    let err = Math.abs(renderedData[k] - reference[k]);
    if (err > maxError) {
      maxError = err;
      maxPosition = k;
    }
    if (!isValidNumber(renderedData[k])) {
      ++invalidNumberCount;
    }
  }

  should(
      invalidNumberCount, 'Number of non-finite values in the rendered output')
      .beEqualTo(0);

  should(maxError, 'Max error in ' + filterTypeName[filterType] + ' response')
      .beLessThanOrEqualTo(maxAllowedError);
}
