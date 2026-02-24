// For cross-thread messaging.
class MessengerProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.port.onmessage = this.startTest.bind(this);
  }

  process() {}

  startTest(message) {
    runRegisterProcessorTest(this.port);
  }
}

function runRegisterProcessorTest(messagePort) {
  try {
    // TypeError when a given parameter is not a Function.
    const DummyObject = {};
    registerProcessor('type-error-on-object', DummyObject);
  } catch (exception) {
    messagePort.postMessage({
      name: exception.name,
      message: exception.message
    });
  }

  try {
    // TypeError When a given parameter is a Function, but not a constructor.
    const DummyFunction = () => {};
    registerProcessor('type-error-on-function', DummyFunction);
  } catch (exception) {
    messagePort.postMessage({
      name: exception.name,
      message: exception.message
    });
  }
}

registerProcessor('messenger-processor', MessengerProcessor);
