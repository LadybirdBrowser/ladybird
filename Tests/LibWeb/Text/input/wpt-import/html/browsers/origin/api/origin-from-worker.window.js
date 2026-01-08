// META: title=`Origin.from(Worker)`

const src = `
const originA = Origin.from(globalThis);
const originB = Origin.from(globalThis);

self.postMessage({
  "isOpaque": originA.opaque,
  "sameOrigin": originA.isSameOrigin(originB),
});
`;

async_test(t => {
  const dataURL = `data:text/html;base64,${btoa(src)}`;
  const worker = new Worker(dataURL);
  worker.onmessage = t.step_func_done(m => {
    assert_true(m.data.isOpaque, "Origin created from data URL Worker should be an opaque origin.");
    assert_true(m.data.sameOrigin, "Two data URL opaque origins should be same-origin with one another.");
  });
}, "Comparison of `Origin.from(Worker)` for opaque data URL origin.");

async_test(t => {
  const blob = new Blob([src], { type: 'application/javascript' });
  const worker = new Worker(URL.createObjectURL(blob));
  worker.onmessage = t.step_func_done(m => {
    assert_false(m.data.isOpaque, "Origin created from Worker should be a tuple origin.");
    assert_true(m.data.sameOrigin, "Two tuple origins should be same-origin with one another.");
  });
}, "Comparison of `Origin.from(Worker)` tuple origins.");
