
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IndexedDB: Test error events fired at requests from aborted transaction";
importScripts("resources/support-promises.js")
importScripts("resources/support.js")
importScripts("/IndexedDB/transaction-abort-request-error.any.js");
done();
