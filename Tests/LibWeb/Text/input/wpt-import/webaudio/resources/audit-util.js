// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


/**
 * @fileOverview  This file includes legacy utility functions for the layout
 *                test.
 */

// How many frames in a WebAudio render quantum.
let RENDER_QUANTUM_FRAMES = 128;

// Compare two arrays (commonly extracted from buffer.getChannelData()) with
// constraints:
//   options.thresholdSNR: Minimum allowed SNR between the actual and expected
//     signal. The default value is 10000.
//   options.thresholdDiffULP: Maximum allowed difference between the actual
//     and expected signal in ULP(Unit in the last place). The default is 0.
//   options.thresholdDiffCount: Maximum allowed number of sample differences
//     which exceeds the threshold. The default is 0.
//   options.bitDepth: The expected result is assumed to come from an audio
//     file with this number of bits of precision. The default is 16.
function compareBuffersWithConstraints(should, actual, expected, options) {
  if (!options)
    options = {};

  // Only print out the message if the lengths are different; the
  // expectation is that they are the same, so don't clutter up the
  // output.
  if (actual.length !== expected.length) {
    should(
        actual.length === expected.length,
        'Length of actual and expected buffers should match')
        .beTrue();
  }

  let maxError = -1;
  let diffCount = 0;
  let errorPosition = -1;
  let thresholdSNR = (options.thresholdSNR || 10000);

  let thresholdDiffULP = (options.thresholdDiffULP || 0);
  let thresholdDiffCount = (options.thresholdDiffCount || 0);

  // By default, the bit depth is 16.
  let bitDepth = (options.bitDepth || 16);
  let scaleFactor = Math.pow(2, bitDepth - 1);

  let noisePower = 0, signalPower = 0;

  for (let i = 0; i < actual.length; i++) {
    let diff = actual[i] - expected[i];
    noisePower += diff * diff;
    signalPower += expected[i] * expected[i];

    if (Math.abs(diff) > maxError) {
      maxError = Math.abs(diff);
      errorPosition = i;
    }

    // The reference file is a 16-bit WAV file, so we will almost never get
    // an exact match between it and the actual floating-point result.
    if (Math.abs(diff) > scaleFactor)
      diffCount++;
  }

  let snr = 10 * Math.log10(signalPower / noisePower);
  let maxErrorULP = maxError * scaleFactor;

  should(snr, 'SNR').beGreaterThanOrEqualTo(thresholdSNR);

  should(
      maxErrorULP,
      options.prefix + ': Maximum difference (in ulp units (' + bitDepth +
          '-bits))')
      .beLessThanOrEqualTo(thresholdDiffULP);

  should(diffCount, options.prefix + ': Number of differences between results')
      .beLessThanOrEqualTo(thresholdDiffCount);
}

// Create an impulse in a buffer of length sampleFrameLength
function createImpulseBuffer(context, sampleFrameLength) {
  let audioBuffer =
      context.createBuffer(1, sampleFrameLength, context.sampleRate);
  let n = audioBuffer.length;
  let dataL = audioBuffer.getChannelData(0);

  for (let k = 0; k < n; ++k) {
    dataL[k] = 0;
  }
  dataL[0] = 1;

  return audioBuffer;
}

// Create a buffer of the given length with a linear ramp having values 0 <= x <
// 1.
function createLinearRampBuffer(context, sampleFrameLength) {
  let audioBuffer =
      context.createBuffer(1, sampleFrameLength, context.sampleRate);
  let n = audioBuffer.length;
  let dataL = audioBuffer.getChannelData(0);

  for (let i = 0; i < n; ++i)
    dataL[i] = i / n;

  return audioBuffer;
}

// Create an AudioBuffer of length |sampleFrameLength| having a constant value
// |constantValue|. If |constantValue| is a number, the buffer has one channel
// filled with that value. If |constantValue| is an array, the buffer is created
// wit a number of channels equal to the length of the array, and channel k is
// filled with the k'th element of the |constantValue| array.
function createConstantBuffer(context, sampleFrameLength, constantValue) {
  let channels;
  let values;

  if (typeof constantValue === 'number') {
    channels = 1;
    values = [constantValue];
  } else {
    channels = constantValue.length;
    values = constantValue;
  }

  let audioBuffer =
      context.createBuffer(channels, sampleFrameLength, context.sampleRate);
  let n = audioBuffer.length;

  for (let c = 0; c < channels; ++c) {
    let data = audioBuffer.getChannelData(c);
    for (let i = 0; i < n; ++i)
      data[i] = values[c];
  }

  return audioBuffer;
}

// Create a stereo impulse in a buffer of length sampleFrameLength
function createStereoImpulseBuffer(context, sampleFrameLength) {
  let audioBuffer =
      context.createBuffer(2, sampleFrameLength, context.sampleRate);
  let n = audioBuffer.length;
  let dataL = audioBuffer.getChannelData(0);
  let dataR = audioBuffer.getChannelData(1);

  for (let k = 0; k < n; ++k) {
    dataL[k] = 0;
    dataR[k] = 0;
  }
  dataL[0] = 1;
  dataR[0] = 1;

  return audioBuffer;
}

// Convert time (in seconds) to sample frames.
function timeToSampleFrame(time, sampleRate) {
  return Math.floor(0.5 + time * sampleRate);
}

// Compute the number of sample frames consumed by noteGrainOn with
// the specified |grainOffset|, |duration|, and |sampleRate|.
function grainLengthInSampleFrames(grainOffset, duration, sampleRate) {
  let startFrame = timeToSampleFrame(grainOffset, sampleRate);
  let endFrame = timeToSampleFrame(grainOffset + duration, sampleRate);

  return endFrame - startFrame;
}

// True if the number is not an infinity or NaN
function isValidNumber(x) {
  return !isNaN(x) && (x != Infinity) && (x != -Infinity);
}

// Compute the (linear) signal-to-noise ratio between |actual| and
// |expected|.  The result is NOT in dB!  If the |actual| and
// |expected| have different lengths, the shorter length is used.
function computeSNR(actual, expected) {
  let signalPower = 0;
  let noisePower = 0;

  let length = Math.min(actual.length, expected.length);

  for (let k = 0; k < length; ++k) {
    let diff = actual[k] - expected[k];
    signalPower += expected[k] * expected[k];
    noisePower += diff * diff;
  }

  return signalPower / noisePower;
}

/**
 * Asserts that all elements in the given array are equal to the specified value
 * If the value is NaN, checks that each element in the array is also NaN.
 * Throws an assertion error if any element does not match the expected value.
 *
 * @param {Array<number>} array - The array of numbers to check.
 * @param {number} value - The constant that each array element should match.
 * @param {string} [messagePrefix=''] - Optional for assertion error messages.
 */
function assert_constant_value(array, value, messagePrefix = '') {
  for (let i = 0; i < array.length; ++i) {
    if (Number.isNaN(value)) {
      assert_true(
        Number.isNaN(array[i]),
        `${messagePrefix} entry ${i} should be NaN`
      );
    } else {
      assert_equals(
        array[i],
        value,
        `${messagePrefix} entry ${i} should be ${value}`
      );
    }
  }
}

/**
 * Asserts that two arrays are exactly equal, element by element.
 * @param {!Array<number>} actual The actual array of values.
 * @param {!Array<number>} expected The expected array of values.
 * @param {string} message Description used for assertion failures.
 */
function assert_array_equals_exact(actual, expected, message) {
  assert_equals(actual.length, expected.length, 'Buffers must be same length');
  for (let i = 0; i < actual.length; ++i) {
    assert_equals(actual[i], expected[i], `${message} (at index ${i})`);
  }
}

/**
 * Asserts that an array is not a constant array
 * (i.e., not all values are equal to the given constant).
 * @param {!Array<number>} array The array to be checked.
 * @param {number} constantValue The constant value to compare against.
 * @param {string} message Description used for assertion failures.
 */
function assert_not_constant_value(array, constantValue, message) {
  const notAllSame = array.some(value => value !== constantValue);
  assert_true(notAllSame, message);
}

/**
 * Asserts that all elements of an array are exactly equal to a constant value.
 * @param {!Array<number>} array The array to be checked.
 * @param {number} constantValue The expected constant value.
 * @param {string} message Description used for assertion failures.
 */
function assert_strict_constant_value(array, constantValue, message) {
  const allSame = array.every(value => value === constantValue);
  assert_true(allSame, message);
}

/**
 * Asserts that all elements of an array are (approximately) equal to a value.
 *
 * @param {!Array<number>} array - The array to be checked.
 * @param {number} constantValue - The expected constant value.
 * @param {string} message - Description used for assertion failures.
 * @param {number=} epsilon - Allowed tolerance for floating-point comparison.
 * Default to 1e-7
 */
function assert_array_constant_value(
    array, constantValue, message, epsilon = 1e-7) {
      for (let i = 0; i < array.length; ++i) {
        assert_approx_equals(
            array[i], constantValue, epsilon, `${message} sample[${i}]`);
      }
}

/**
 * Asserts that two arrays are equal within a given tolerance for each element.
 * The |threshold| can be:
 *   - A number (absolute epsilon)
 *   - An object with optional {absoluteThreshold, relativeThreshold}
 *   - If omitted, compares with exact equality (epsilon = 0)
 *
 * For each element i, we require:
 *   |actual[i] − expected[i]| ≤ max(absoluteThreshold,
 *                                   |expected[i]|·relativeThreshold)
 *
 * @param {!Array<number>|!TypedArray<number>} actual
 * @param {!Array<number>|!TypedArray<number>} expected
 * @param {number|{ absoluteThreshold?:number, relativeThreshold?:number }}
 *   [threshold=0]
 * @param {string} desc
 */
function assert_array_equal_within_eps(
    actual, expected, threshold = 0, desc) {
  assert_equals(actual.length, expected.length, desc + ': length mismatch');

  let abs = 0;
  let rel = 0;

  if (typeof threshold === 'number') {
    abs = threshold;
  } else if (threshold && typeof threshold === 'object') {
    abs = threshold.absoluteThreshold ?? 0;
    rel = threshold.relativeThreshold ?? 0;
  }

  for (let i = 0; i < actual.length; ++i) {
    const epsilon = Math.max(abs, Math.abs(expected[i]) * rel);
    const diff = Math.abs(actual[i] - expected[i]);
    assert_approx_equals(
        actual[i],
        expected[i],
        epsilon,
        `${desc} sample[${i}] |${actual[i]} - ${expected[i]}|` +
            ` = ${diff} > ${epsilon}`);
  }
}

function assert_not_constant_value_of(array, value, description) {
  assert_true(array && typeof array.length === 'number' && array.length > 0,
      `${description}: input must be a non-empty array`);

  const hasDifferentValues = array.some(element => element !== value);

  // Pass if there's at least one element not strictly equal to `value`.
  assert_true(
      hasDifferentValues,
      `${description}: ${array} should have contained at least `+
          `one value different from ${value}.`
  );
}
