/**
 * @class ProcessGetterTestPrototypeProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class tests that a 'process' getter on
 * AudioWorkletProcessorConstructor is called at the right times.
 */

// Reporting errors during registerProcess() is awkward.
// The occurrance of an error is flagged, so that a trial registration can be
// performed and registration against the expected AudioWorkletNode name is
// performed only if no errors are flagged during the trial registration.
let error_flag = false;

class ProcessGetterTestPrototypeProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.getterCallCount = 0;
    this.totalProcessCallCount = 0;
  }
  get process() {
    if (!(this instanceof ProcessGetterTestPrototypeProcessor)) {
      error_flag = true;
      throw new Error('`process` getter called with bad `this`.');
    }
    ++this.getterCallCount;
    let functionCallCount = 0;
    return () => {
      if (++functionCallCount > 1) {
        const message = 'Closure of function returned from `process` getter' +
            ' should be used for only one call.'
        this.port.postMessage({message: message});
        throw new Error(message);
      }
      if (++this.totalProcessCallCount < 2) {
        return true; // Expect another getter call.
      }
      if (this.totalProcessCallCount != this.getterCallCount) {
        const message =
            'Getter should be called only once for each process() call.'
        this.port.postMessage({message: message});
        throw new Error(message);
      }
      this.port.postMessage({message: 'done'});
      return false; // No more calls required.
    };
  }
}

registerProcessor('trial-process-getter-test-prototype',
                  ProcessGetterTestPrototypeProcessor);
if (!error_flag) {
  registerProcessor('process-getter-test-prototype',
                    ProcessGetterTestPrototypeProcessor);
}
