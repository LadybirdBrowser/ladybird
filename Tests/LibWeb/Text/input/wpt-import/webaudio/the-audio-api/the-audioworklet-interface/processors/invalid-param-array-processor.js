/**
 * @class InvalidParamArrayProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor intentionally returns an array with an invalid size when the
 * processor's getter is queried.
 */
let singleton = undefined;
let secondFetch = false;
let useDescriptor = false;
let processCounter = 0;

class InvalidParamArrayProcessor extends AudioWorkletProcessor {
  static get parameterDescriptors() {
    if (useDescriptor)
      return [{name: 'invalidParam'}];
    useDescriptor = true;
    return [];
  }

  constructor() {
    super();
    if (singleton === undefined)
      singleton = this;
    return singleton;
  }

  process(inputs, outputs, parameters) {
    const output = outputs[0];
    for (let channel = 0; channel < output.length; ++channel)
      output[channel].fill(1);
    return false;
  }
}

// This overridden getter is invoked under the hood before process() gets
// called. After this gets called, process() method above will be invalidated,
// and mark the worklet node non-functional. (i.e. in an error state)
Object.defineProperty(Object.prototype, 'invalidParam', {'get': () => {
  if (secondFetch)
    return new Float32Array(256);
  secondFetch = true;
  return new Float32Array(128);
}});

registerProcessor('invalid-param-array-1', InvalidParamArrayProcessor);
registerProcessor('invalid-param-array-2', InvalidParamArrayProcessor);
