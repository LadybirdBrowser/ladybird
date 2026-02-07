class NewAfterSuper extends AudioWorkletProcessor {
  constructor() {
    super()
    let message = {threw: false};
    try {
      new AudioWorkletProcessor()
    } catch (e) {
      message.threw = true;
      message.errorName = e.name;
      message.isTypeError = e instanceof TypeError;
    }
    this.port.postMessage(message);
  }
}
registerProcessor("new-after-super", NewAfterSuper);
