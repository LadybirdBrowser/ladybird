/**
 * @class ParamSizeProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor is a source node which basically outputs the size of the
 * AudioParam array for each render quantum.
 */

class ParamSizeProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    return [{name: 'param'}];
  }

  constructor() {
    super();
  }

  process(inputs, outputs, parameters) {
    let output = outputs[0];
    let param = parameters.param;

    for (let channel = 0; channel < output.length; ++channel) {
      output[channel].fill(param.length);
    }

    return true;
  }
}

registerProcessor('param-size', ParamSizeProcessor);
