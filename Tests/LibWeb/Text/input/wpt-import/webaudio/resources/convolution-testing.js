let sampleRate = 44100.0;

let renderLengthSeconds = 8;
let pulseLengthSeconds = 1;
let pulseLengthFrames = pulseLengthSeconds * sampleRate;

function createSquarePulseBuffer(context, sampleFrameLength) {
  let audioBuffer =
      context.createBuffer(1, sampleFrameLength, context.sampleRate);

  let n = audioBuffer.length;
  let data = audioBuffer.getChannelData(0);

  for (let i = 0; i < n; ++i)
    data[i] = 1;

  return audioBuffer;
}

// The triangle buffer holds the expected result of the convolution.
// It linearly ramps up from 0 to its maximum value (at the center)
// then linearly ramps down to 0.  The center value corresponds to the
// point where the two square pulses overlap the most.
function createTrianglePulseBuffer(context, sampleFrameLength) {
  let audioBuffer =
      context.createBuffer(1, sampleFrameLength, context.sampleRate);

  let n = audioBuffer.length;
  let halfLength = n / 2;
  let data = audioBuffer.getChannelData(0);

  for (let i = 0; i < halfLength; ++i)
    data[i] = i + 1;

  for (let i = halfLength; i < n; ++i)
    data[i] = n - i - 1;

  return audioBuffer;
}

function log10(x) {
  return Math.log(x) / Math.LN10;
}

function linearToDecibel(x) {
  return 20 * log10(x);
}

// Verify that the rendered result is very close to the reference
// triangular pulse.
function checkTriangularPulse(rendered, reference, should) {
  let match = true;
  let maxDelta = 0;
  let valueAtMaxDelta = 0;
  let maxDeltaIndex = 0;

  for (let i = 0; i < reference.length; ++i) {
    let diff = rendered[i] - reference[i];
    let x = Math.abs(diff);
    if (x > maxDelta) {
      maxDelta = x;
      valueAtMaxDelta = reference[i];
      maxDeltaIndex = i;
    }
  }

  // allowedDeviationFraction was determined experimentally.  It
  // is the threshold of the relative error at the maximum
  // difference between the true triangular pulse and the
  // rendered pulse.
  let allowedDeviationDecibels = -124.41;
  let maxDeviationDecibels = linearToDecibel(maxDelta / valueAtMaxDelta);

  should(
      maxDeviationDecibels,
      'Deviation (in dB) of triangular portion of convolution')
      .beLessThanOrEqualTo(allowedDeviationDecibels);

  return match;
}

// Verify that the rendered data is close to zero for the first part
// of the tail.
function checkTail1(data, reference, breakpoint, should) {
  let isZero = true;
  let tail1Max = 0;

  for (let i = reference.length; i < reference.length + breakpoint; ++i) {
    let mag = Math.abs(data[i]);
    if (mag > tail1Max) {
      tail1Max = mag;
    }
  }

  // Let's find the peak of the reference (even though we know a
  // priori what it is).
  let refMax = 0;
  for (let i = 0; i < reference.length; ++i) {
    refMax = Math.max(refMax, Math.abs(reference[i]));
  }

  // This threshold is experimentally determined by examining the
  // value of tail1MaxDecibels.
  let threshold1 = -129.7;

  let tail1MaxDecibels = linearToDecibel(tail1Max / refMax);
  should(tail1MaxDecibels, 'Deviation in first part of tail of convolutions')
      .beLessThanOrEqualTo(threshold1);

  return isZero;
}

// Verify that the second part of the tail of the convolution is
// exactly zero.
function checkTail2(data, reference, breakpoint, should) {
  let isZero = true;
  let tail2Max = 0;
  // For the second part of the tail, the maximum value should be
  // exactly zero.
  let threshold2 = 0;
  for (let i = reference.length + breakpoint; i < data.length; ++i) {
    if (Math.abs(data[i]) > 0) {
      isZero = false;
      break;
    }
  }

  should(isZero, 'Rendered signal after tail of convolution is silent')
      .beTrue();

  return isZero;
}

function checkConvolvedResult(renderedBuffer, trianglePulse, should) {
  let referenceData = trianglePulse.getChannelData(0);
  let renderedData = renderedBuffer.getChannelData(0);

  let success = true;

  // Verify the triangular pulse is actually triangular.

  success =
      success && checkTriangularPulse(renderedData, referenceData, should);

  // Make sure that portion after convolved portion is totally
  // silent.  But round-off prevents this from being completely
  // true.  At the end of the triangle, it should be close to
  // zero.  If we go farther out, it should be even closer and
  // eventually zero.

  // For the tail of the convolution (where the result would be
  // theoretically zero), we partition the tail into two
  // parts.  The first is the at the beginning of the tail,
  // where we tolerate a small but non-zero value.  The second part is
  // farther along the tail where the result should be zero.

  // breakpoint is the point dividing the first two tail parts
  // we're looking at.  Experimentally determined.
  let breakpoint = 12800;

  success =
      success && checkTail1(renderedData, referenceData, breakpoint, should);

  success =
      success && checkTail2(renderedData, referenceData, breakpoint, should);

  should(success, 'Test signal convolved').message('correctly', 'incorrectly');
}
