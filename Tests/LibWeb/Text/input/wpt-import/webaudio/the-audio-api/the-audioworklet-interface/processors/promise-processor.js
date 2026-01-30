/**
 * @class PromiseProcessor
 * @extends AudioWorkletProcessor
 *
 * This processor creates and resolves a promise in its `process` method. When
 * the handler passed to `then()` is called, a counter that is global in the
 * global scope is incremented. There are two copies of this
 * AudioWorkletNode/Processor, so the counter should always be even in the
 * process method of the AudioWorklet processing, since the Promise completion
 * handler are resolved in between render quanta.
 *
 * After a few iterations of the test, one of the worklet posts back the string
 * "ok" to the main thread, and the test is considered a success.
 */
var idx = 0;

class PromiseProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super(options);
  }

  process(inputs, outputs) {
    if (idx % 2 != 0) {
      this.port.postMessage("ko");
      // Don't bother continuing calling process in this case, the test has
      // already failed.
      return false;
    }
    Promise.resolve().then(() => {
      idx++;
      if (idx == 100) {
        this.port.postMessage("ok");
      }
    });
    // Ensure process is called again.
    return true;
  }
}

registerProcessor('promise-processor', PromiseProcessor);
