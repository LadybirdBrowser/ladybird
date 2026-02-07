class ProcessorA extends AudioWorkletProcessor {
  process() {
    return true;
  }
}

// ProcessorB registers ProcessorA upon the construction.
class ProcessorB extends AudioWorkletProcessor {
  constructor() {
    super();
    this.port.onmessage = () => {
      registerProcessor('ProcessorA', ProcessorA);
      this.port.postMessage({});
    };
  }

  process() {
    return true;
  }
}

registerProcessor('ProcessorB', ProcessorB);
