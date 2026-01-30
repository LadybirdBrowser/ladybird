/**
 * @class TimingInfoProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class is to test the timing information in AWGS.
 */
class TimingInfoProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.port.onmessage = this.echoMessage.bind(this);
  }

  echoMessage(event) {
    this.port.postMessage({
      currentTime: currentTime,
      currentFrame: currentFrame
    });
  }

  process() {
    return true;
  }
}

registerProcessor('timing-info-processor', TimingInfoProcessor);
