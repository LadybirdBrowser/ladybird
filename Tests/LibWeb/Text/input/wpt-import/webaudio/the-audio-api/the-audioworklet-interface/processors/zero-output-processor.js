/**
 * @class ZeroOutputProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor accumulates the incoming buffer and send the buffered data
 * to the main thread when it reaches the specified frame length. The processor
 * only supports the single input.
 */

const kRenderQuantumFrames = 128;

class ZeroOutputProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();

    this._framesRequested = options.processorOptions.bufferLength;
    this._framesCaptured = 0;
    this._buffer = [];
    for (let i = 0; i < options.processorOptions.channeCount; ++i) {
      this._buffer[i] = new Float32Array(this._framesRequested);
    }
  }

  process(inputs) {
    let input = inputs[0];
    let startIndex = this._framesCaptured;
    let endIndex = startIndex + kRenderQuantumFrames;
    for (let i = 0; i < this._buffer.length; ++i) {
      this._buffer[i].subarray(startIndex, endIndex).set(input[i]);
    }
    this._framesCaptured = endIndex;

    if (this._framesCaptured >= this._framesRequested) {
      this.port.postMessage({ capturedBuffer: this._buffer });
      return false;
    } else {
      return true;
    }
  }
}

registerProcessor('zero-output-processor', ZeroOutputProcessor);
