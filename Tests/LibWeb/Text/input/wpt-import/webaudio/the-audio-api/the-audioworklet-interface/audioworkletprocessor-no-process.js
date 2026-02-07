'use strict';

/**
 * @class NoProcessDef
 * @extends AudioWorkletProcessor
 *
 * This processor class demonstrates an AudioWorkletProcessor with no
 * process named function defined.
 */
class NoProcessDef extends AudioWorkletProcessor {
  constructor() {
    super();
    this.port.postMessage({
      state: 'created',
    });
  }
}

registerProcessor('audioworkletprocessor-no-process', NoProcessDef);
