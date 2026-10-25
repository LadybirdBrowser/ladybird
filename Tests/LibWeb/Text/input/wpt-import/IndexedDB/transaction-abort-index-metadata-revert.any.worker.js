
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IndexedDB: aborting transactions reverts index metadata";
importScripts("resources/support-promises.js")
importScripts("resources/support.js")
importScripts("/IndexedDB/transaction-abort-index-metadata-revert.any.js");
done();
