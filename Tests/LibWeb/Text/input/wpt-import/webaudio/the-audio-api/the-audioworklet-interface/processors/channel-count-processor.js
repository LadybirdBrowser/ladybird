/**
 * @class ChannelCountProcessor
 * @extends AudioWorkletProcessor
 */
class ChannelCountProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super(options);
  }

  process(inputs, outputs) {
    this.port.postMessage({
      inputChannel: inputs[0].length,
      outputChannel: outputs[0].length
    });
    return false;
  }
}

registerProcessor('channel-count', ChannelCountProcessor);