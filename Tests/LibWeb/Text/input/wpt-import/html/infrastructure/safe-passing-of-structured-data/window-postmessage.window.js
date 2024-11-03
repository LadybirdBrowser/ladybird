// META: script=/common/sab.js
// META: script=/html/webappapis/structured-clone/structured-clone-battery-of-tests.js
// META: script=/html/webappapis/structured-clone/structured-clone-battery-of-tests-with-transferables.js
// META: script=/html/webappapis/structured-clone/structured-clone-battery-of-tests-harness.js

runStructuredCloneBatteryOfTests({
  structuredClone(data, transfer) {
    return new Promise(resolve => {
      window.addEventListener('message', function f(ev) {
        window.removeEventListener('message', f);
        resolve(ev.data.data);
      });
      window.postMessage({data, transfer}, "/", transfer);
    });
  }
});
