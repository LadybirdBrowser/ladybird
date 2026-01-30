'use strict';

// This value is used to set the values in an AudioParam.
const TestValue = 0.5;

// Prepare 4 outputs; 2 outputs will be unconnected for testing.
const WorkletNodeOptions =  {
  processorOptions: {testValue: TestValue},
  numberOfInputs: 0,
  numberOfOutputs: 4
};

// The code for the AWP definition in AudioWorkletGlobalScope.
const processorCode = () => {

  // This processor sends the `outputs` array to the main thread at the first
  // process call - after filling its 2nd output with the test value.
  class OutputTestProcessor extends AudioWorkletProcessor {

    constructor(options) {
      super(options);
      this.testValue = options.processorOptions.testValue;
    }

    process(inputs, outputs) {
      // Fill the second output of this process with the `testValue`.
      const output = outputs[1];
      for (const channel of output) {
        channel.fill(this.testValue);
      }

      // Send the outputs array and stop rendering.
      this.port.postMessage({outputs});
      return false;
    }
  }

  registerProcessor('output-test-processor', OutputTestProcessor);

  // This process has an AudioParam and sends the `params` array to the main
  // thread at the first process call.
  class ParamTestProcessor extends AudioWorkletProcessor {
    static get parameterDescriptors() {
      return [
        {name: 'testParam', defaultValue: 0.0}
      ];
    }

    process(inputs, outputs, params) {
      // Send the params array and stop rendering.
      this.port.postMessage({paramValues: params.testParam});
      return false;
    }
  }

  registerProcessor('param-test-processor', ParamTestProcessor);
}

const initializeAudioContext = async () => {
  const context = new AudioContext();
  const moduleString = `(${processorCode.toString()})();`;
  const blobUrl = window.URL.createObjectURL(
      new Blob([moduleString], {type: 'text/javascript'}));
  await context.audioWorklet.addModule(blobUrl);
  context.suspend();
  return context;
};

// Test if unconnected outputs provides a non-zero length array for channels.
promise_test(async () => {
  const context = await initializeAudioContext();
  const outputTester = new AudioWorkletNode(
      context, 'output-test-processor', WorkletNodeOptions);
  const testGain = new GainNode(context);

  // Connect the 2nd output of the tester to another node. Note that
  // `testGain` is not connected to the destination.
  outputTester.connect(testGain, 1);

  // Connect the 4th output of the tester to the destination node.
  outputTester.connect(context.destination, 3);

  return new Promise(resolve => {
    outputTester.port.onmessage = resolve;
    context.resume();
  }).then(event => {
    // The number of outputs should be 4, as specified above.
    const outputs = event.data.outputs;
    assert_equals(outputs.length, WorkletNodeOptions.numberOfOutputs);
    for (const output of outputs) {
      // Each output should have 1 channel of audio data per spec.
      assert_equals(output.length, 1);
      for (const channel of output) {
        // Each channel should have a non-zero length array.
        assert_true(channel.length > 0);
      }
    }
    context.close();
  });
}, 'Test if unconnected outputs provides a non-zero length array for channels');

// Test if outputs connected to AudioParam provides a non-zero length array for
// channels.
promise_test(async () => {
  const context = await initializeAudioContext();
  const outputTester = new AudioWorkletNode(
      context, 'output-test-processor', WorkletNodeOptions);
  const paramTester = new AudioWorkletNode(
      context, 'param-test-processor');

  // Connect the 2nd output of the tester to another node's AudioParam.
  outputTester.connect(paramTester.parameters.get('testParam'), 1);

  outputTester.connect(context.destination);

  return new Promise(resolve => {
    paramTester.port.onmessage = resolve;
    context.resume();
  }).then(event => {
    // The resulting values from AudioParam should be a non-zero length array
    // filled with `TestValue` above.
    const actualValues = event.data.paramValues;
    const expectedValues = (new Array(actualValues.length)).fill(TestValue);
    assert_true(actualValues.length > 0);
    assert_array_equals(actualValues, expectedValues);
    context.close();
  });
}, 'Test if outputs connected to AudioParam provides a non-zero length array ' +
   'for channels');
