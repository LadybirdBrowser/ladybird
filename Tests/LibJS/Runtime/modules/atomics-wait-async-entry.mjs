const array = new Int32Array(new SharedArrayBuffer(4));
globalThis.asyncWaitModuleResult = await Atomics.waitAsync(array, 0, 0, 1).value;
