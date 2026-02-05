// META: global=window,worker
// META: script=/common/sab.js
// META: script=/html/webappapis/structured-clone/structured-clone-battery-of-tests.js
// META: script=/html/webappapis/structured-clone/structured-clone-battery-of-tests-with-transferables.js
// META: script=/html/webappapis/structured-clone/structured-clone-battery-of-tests-harness.js

runStructuredCloneBatteryOfTests({
  structuredClone(data, transfer) {
    return new Promise(resolve => {
      const channel = new MessageChannel();
      channel.port2.onmessage = ev => resolve(ev.data.data);
      channel.port1.postMessage({data, transfer}, transfer);
    });
  },
  hasDocument : self.GLOBAL.isWindow()
});
