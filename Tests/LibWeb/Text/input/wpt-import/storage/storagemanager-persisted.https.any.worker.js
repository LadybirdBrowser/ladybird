
self.GLOBAL = {
  isWindow: function() { return false; },
  isWorker: function() { return true; },
  isShadowRealm: function() { return false; },
};
importScripts("/resources/testharness.js");
self.META_TITLE = "StorageManager: persisted()";
importScripts("/storage/storagemanager-persisted.https.any.js");
done();
