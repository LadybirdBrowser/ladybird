/**
 * @class CountProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class just looks at the number of input channels on the first
 * input and fills the first output channel with that value.
 */
class CountProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
  }

  process(inputs, outputs, parameters) {
    let input = inputs[0];
    let output = outputs[0];
    output[0].fill(input.length);

    return true;
  }
}

registerProcessor('counter', CountProcessor);
