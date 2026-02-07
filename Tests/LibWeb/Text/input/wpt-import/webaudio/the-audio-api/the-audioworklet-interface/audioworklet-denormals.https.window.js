'use strict';

// Test if the JS code execution in AudioWorkletGlobalScope can handle the
// denormals properly. For more details, see:
// https://esdiscuss.org/topic/float-denormal-issue-in-javascript-processor-node-in-web-audio-api
promise_test(async () => {
  // In the main thread, the denormals should be non-zeros.
  assert_not_equals(Number.MIN_VALUE, 0.0,
                    'The denormals should be non-zeros.');

  const context = new AudioContext();
  await context.audioWorklet.addModule(
      './processors/denormal-test-processor.js');

  const denormalTestProcessor = new AudioWorkletNode(context, 'denormal-test');

  return new Promise(resolve => {
    denormalTestProcessor.port.onmessage = resolve;
    denormalTestProcessor.connect(context.destination);
  }).then(event => {
    // In the AudioWorkletGlobalScope, the denormals should be non-zeros too.
    assert_true(
        event.data.result,
        'The denormals should be non-zeros in AudioWorkletGlobalScope.');
  });
}, 'Test denormal behavior in AudioWorkletGlobalScope');
