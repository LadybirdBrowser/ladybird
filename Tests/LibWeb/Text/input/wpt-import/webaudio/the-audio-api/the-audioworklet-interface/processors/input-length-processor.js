/**
 * @class InputLengthProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class just sets the output to the length of the
 * input array for verifying that the input length changes when the
 * input is disconnected.
 */
class InputLengthProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
  }

  process(inputs, outputs, parameters) {
    let input = inputs[0];
    let output = outputs[0];

    // Set output channel to the length of the input channel array.
    // If the input is unconnected, set the value to zero.
    const fillValue = input.length > 0 ? input[0].length : 0;
    output[0].fill(fillValue);

    return true;
  }
}

registerProcessor('input-length-processor', InputLengthProcessor);
