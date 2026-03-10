/**
 * @class ActiveProcessingTester
 * @extends AudioWorkletProcessor
 *
 * This processor class sends a message to its AudioWorkletNodew whenever the
 * number of channels on the input changes.  The message includes the actual
 * number of channels, the context time at which this occurred, and whether
 * we're done processing or not.
 */
class ActiveProcessingTester extends AudioWorkletProcessor {
  constructor(options) {
    super(options);
    this._lastChannelCount = 0;

    // See if user specified a value for test duration.
    if (options.hasOwnProperty('processorOptions') &&
        options.processorOptions.hasOwnProperty('testDuration')) {
      this._testDuration = options.processorOptions.testDuration;
    } else {
      this._testDuration = 5;
    }

    // Time at which we'll signal we're done, based on the requested
    // |testDuration|
    this._endTime = currentTime + this._testDuration;
  }

  process(inputs, outputs) {
    const input = inputs[0];
    const output = outputs[0];
    const inputChannelCount = input.length;
    const isFinished = currentTime > this._endTime;

    // Send a message if we're done or the count changed.
    if (isFinished || (inputChannelCount != this._lastChannelCount)) {
      this.port.postMessage({
        channelCount: inputChannelCount,
        finished: isFinished,
        time: currentTime
      });
      this._lastChannelCount = inputChannelCount;
    }

    // Just copy the input to the output for no particular reason.
    for (let channel = 0; channel < input.length; ++channel) {
      output[channel].set(input[channel]);
    }

    // When we're finished, this method no longer needs to be called.
    return !isFinished;
  }
}

registerProcessor('active-processing-tester', ActiveProcessingTester);
