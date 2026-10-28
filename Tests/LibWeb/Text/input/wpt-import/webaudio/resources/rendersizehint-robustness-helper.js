// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

const ROBUSTNESS_TEST_CONFIGS = [
  {sampleRate: 3000, renderSizeHint: 1, length: 512},
  {sampleRate: 48000, renderSizeHint: 256, length: 1024},
  {sampleRate: 48000, renderSizeHint: 100, length: 1024},
  {sampleRate: 192000, renderSizeHint: 4096, length: 8192},
  {sampleRate: 11000, renderSizeHint: 66000, length: 132000}
];

/**
 * Asserts that the rendered AudioBuffer's first channel does not contain
 * NaN or Infinities.
 *
 * @param {AudioBuffer} renderedBuffer The buffer returned from rendering.
 */
const assertNoNaNOrInfinity = (renderedBuffer) => {
  const data = renderedBuffer.getChannelData(0);
  assert_false(data.includes(NaN), 'Output contains NaN');
  assert_false(data.includes(Infinity), 'Output contains Infinity');
  assert_false(data.includes(-Infinity), 'Output contains -Infinity');
};

/**
 * Creates a unit impulse response AudioBuffer of a specified length.
 *
 * @param {BaseAudioContext} context The audio context.
 * @param {number} length The length of the impulse response in frames.
 * @returns {AudioBuffer} The unit impulse AudioBuffer.
 */
const createImpulseResponse = (context, length) => {
  const buffer = new AudioBuffer({
    numberOfChannels: 1,
    length: length,
    sampleRate: context.sampleRate
  });
  const channelData = buffer.getChannelData(0);
  channelData[0] = 1.0; // Unit impulse at sample 0
  return buffer;
};

/**
 * Runs a robustness/stability test for a given node processor.
 *
 * @param {object} config The test configuration containing sampleRate,
 *     renderSizeHint, and length.
 * @param {function(OfflineAudioContext): AudioNode} createTestSetupFunc A
 *     factory function to create and setup the processor node under test.
 * @param {string} testName The name of the test case.
 * @param {function(OfflineAudioContext, AudioBuffer, object): void}
 *     [postRenderAssertFunc] An optional hook to run assertions after
 *     rendering completes, receiving the audioContext, the rendered
 *     AudioBuffer, and the test context.
 */
const runQuantumRobustnessTest = (
    config, createTestSetupFunc, testName, postRenderAssertFunc) => {
  const {sampleRate, renderSizeHint, length} = config;

  promise_test(async (t) => {
    const audioContext = new OfflineAudioContext({
      numberOfChannels: 1,
      length,
      sampleRate,
      renderSizeHint
    });

    const source = new ConstantSourceNode(audioContext);
    const node = createTestSetupFunc(audioContext, t);
    if (!node) {
      return;
    }

    source.connect(node).connect(audioContext.destination);
    source.start();

    const renderedBuffer = await audioContext.startRendering();

    // Verify that the audio output contains no invalid floating-point values
    // (NaN or Infinities), indicating stability under custom block sizes.
    assertNoNaNOrInfinity(renderedBuffer);

    if (postRenderAssertFunc) {
      postRenderAssertFunc(audioContext, renderedBuffer, t);
    }
  }, testName);
};
