class SuperAfterNew extends AudioWorkletProcessor {
  constructor() {
    const processor = new AudioWorkletProcessor()
    let message = {threw: false};
    try {
      super();
    } catch (e) {
      message.threw = true;
      message.errorName = e.name;
      message.isTypeError = e instanceof TypeError;
    }
    processor.port.postMessage(message);
    return processor;
  }
}
registerProcessor("super-after-new", SuperAfterNew);
