// META: global=window,serviceworker

test(() => {
  // See https://github.com/whatwg/html/issues/5380 for why not `new SharedArrayBuffer()`
  const sab = new WebAssembly.Memory({ shared:true, initial:1, maximum:1 }).buffer;
  const ta = new Int32Array(sab);

  assert_throws_js(TypeError, () => {
    Atomics.wait(ta, 0, 0, 10);
  });
}, `[[CanBlock]] in a ${self.constructor.name}`);
