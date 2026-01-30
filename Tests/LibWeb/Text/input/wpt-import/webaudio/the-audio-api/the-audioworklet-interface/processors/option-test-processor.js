/**
 * @class OptionTestProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class demonstrates the option passing feature by echoing the
 * received |nodeOptions| back to the node.
 */
class OptionTestProcessor extends AudioWorkletProcessor {
  constructor(nodeOptions) {
    super();
    this.port.postMessage(nodeOptions);
  }

  process() {
    return true;
  }
}

registerProcessor('option-test-processor', OptionTestProcessor);
