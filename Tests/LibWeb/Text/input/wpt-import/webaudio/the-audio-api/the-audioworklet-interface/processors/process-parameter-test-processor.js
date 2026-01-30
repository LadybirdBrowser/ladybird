/**
 * @class ProcessParameterTestProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class forwards input and output parameters to its
 * AudioWorkletNode.
 */
class ProcessParameterTestProcessor extends AudioWorkletProcessor {
  process(inputs, outputs) {
    this.port.postMessage({
      inputs: inputs,
      outputs: outputs
    });
    return false;
  }
}

registerProcessor('process-parameter-test', ProcessParameterTestProcessor);
