
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IDBObjectStore.createIndex()";
importScripts("resources/support.js")
importScripts("/IndexedDB/idbobjectstore_createIndex.any.js");
done();
