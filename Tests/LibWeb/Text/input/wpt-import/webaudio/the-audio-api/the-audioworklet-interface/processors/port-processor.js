/**
 * @class PortProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class demonstrates the message port functionality.
 */
class PortProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.port.onmessage = this.handleMessage.bind(this);
    this.port.postMessage({
      state: 'created',
      timeStamp: currentTime,
      currentFrame: currentFrame
    });
    this.processCallCount = 0;
  }

  handleMessage(event) {
    this.port.postMessage({
      message: event.data,
      timeStamp: currentTime,
      currentFrame: currentFrame,
      processCallCount: this.processCallCount
    });
  }

  process() {
    ++this.processCallCount;
    return true;
  }
}

registerProcessor('port-processor', PortProcessor);

port.onmessage = (event) => port.postMessage(event.data);
