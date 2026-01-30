/**
 * @class GainProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class demonstrates the bare-bone structure of the processor.
 */
class GainProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    return [
      {name: 'gain', defaultValue: 0.707}
    ];
  }

  constructor() {
    super();
  }

  process(inputs, outputs, parameters) {
    let input = inputs[0];
    let output = outputs[0];
    let gain = parameters.gain;
    for (let channel = 0; channel < input.length; ++channel) {
      let inputChannel = input[channel];
      let outputChannel = output[channel];
      if (gain.length === 1) {
        for (let i = 0; i < inputChannel.length; ++i)
          outputChannel[i] = inputChannel[i] * gain[0];
      } else {
        for (let i = 0; i < inputChannel.length; ++i)
          outputChannel[i] = inputChannel[i] * gain[i];
      }
    }

    return true;
  }
}

registerProcessor('gain', GainProcessor);
