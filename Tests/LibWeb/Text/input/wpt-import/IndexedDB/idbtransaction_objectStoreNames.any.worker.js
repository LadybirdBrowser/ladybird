
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "IndexedDB: IDBTransaction.objectStoreNames attribute";
importScripts("resources/support.js")
importScripts("/IndexedDB/idbtransaction_objectStoreNames.any.js");
done();
