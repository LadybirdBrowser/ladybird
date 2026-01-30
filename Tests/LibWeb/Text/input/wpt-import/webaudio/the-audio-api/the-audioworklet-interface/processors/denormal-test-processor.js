class DenormalTestProcessor extends AudioWorkletProcessor {
  process() {
    // The denormals should be non-zeros. Otherwise, it's a violation of
    // ECMA specification: https://tc39.es/ecma262/#sec-number.min_value
    this.port.postMessage({
      result: Number.MIN_VALUE !== 0.0
    });
    return false;
  }
}

registerProcessor('denormal-test', DenormalTestProcessor);
