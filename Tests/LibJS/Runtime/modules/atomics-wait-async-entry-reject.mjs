const array = new Int32Array(new SharedArrayBuffer(4));
await Atomics.waitAsync(array, 0, 0, 1).value;
throw new Error("module failed after waiting");
