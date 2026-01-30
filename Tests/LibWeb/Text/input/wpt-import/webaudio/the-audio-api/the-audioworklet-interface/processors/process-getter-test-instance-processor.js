/**
 * @class ProcessGetterTestInstanceProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor class tests that a 'process' getter on an
 * AudioWorkletProcessorConstructor instance is called at the right times.
 */

class ProcessGetterTestInstanceProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.getterCallCount = 0;
    this.totalProcessCallCount = 0;
    Object.defineProperty(this, 'process', { get: function() {
      if (!(this instanceof ProcessGetterTestInstanceProcessor)) {
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
    }});
  }
}

registerProcessor('process-getter-test-instance',
                  ProcessGetterTestInstanceProcessor);
